#include "ui/SemanticSearchDialog.h"

#include "core/HumanQuantity.h"
#include "db/Database.h"
#include "features/FeatureStore.h"
#include "features/QueryEmbedding.h"
#include "features/QueryVectorCache.h"

#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace {

constexpr int kResultLimit = 50;
constexpr int kCandidateLimit = 500;
constexpr int kProviderTimeoutMs = 120'000;

enum Column { ColRank = 0, ColScore, ColTitle, ColArtist, ColAlbum, ColYear, ColLength, ColFormat, ColRating, ColCount };

} // namespace

SemanticSearchDialog::SemanticSearchDialog(QString libraryDbPath, QString featuresDbPath, QWidget *parent)
    : QDialog(parent)
    , m_libraryDbPath(std::move(libraryDbPath))
    , m_featuresDbPath(std::move(featuresDbPath))
{
    setWindowTitle(tr("Semantic search"));
    resize(980, 560);

    auto *layout = new QVBoxLayout(this);

    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(tr("Describe the music: warm piano with brushed drums, aggressive breakbeats, ..."));
    m_query->setClearButtonEnabled(true);
    layout->addWidget(m_query);

    m_status = new QLabel(tr("Type a description and press Enter. Matching runs on the analyzed library."), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_results = new QTreeWidget(this);
    m_results->setColumnCount(ColCount);
    m_results->setHeaderLabels({tr("#"), tr("Match"), tr("Title"), tr("Artist"), tr("Album"),
                                tr("Year"), tr("Length"), tr("Format"), tr("Rating")});
    m_results->setRootIsDecorated(false);
    m_results->setUniformRowHeights(true);
    m_results->setAlternatingRowColors(true);
    m_results->setAllColumnsShowFocus(true);
    m_results->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_results->header()->setStretchLastSection(false);
    m_results->header()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    m_results->header()->setSectionResizeMode(ColArtist, QHeaderView::Stretch);
    m_results->header()->setSectionResizeMode(ColAlbum, QHeaderView::Stretch);
    layout->addWidget(m_results, 1);

    auto *buttons = new QHBoxLayout;
    m_playNow = new QPushButton(tr("Play now"), this);
    m_playNext = new QPushButton(tr("Play next"), this);
    m_enqueue = new QPushButton(tr("Add to queue"), this);
    for (QPushButton *button : {m_playNow, m_playNext, m_enqueue}) {
        button->setEnabled(false);
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    auto *close = new QPushButton(tr("Close"), this);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_query, &QLineEdit::returnPressed, this, &SemanticSearchDialog::submitQuery);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_results, &QTreeWidget::itemSelectionChanged, this, [this]() {
        const bool any = !m_results->selectedItems().isEmpty();
        m_playNow->setEnabled(any);
        m_playNext->setEnabled(any);
        m_enqueue->setEnabled(any);
    });
    connect(m_results, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *, int) {
        emitForSelection(&SemanticSearchDialog::playNowRequested);
    });
    connect(m_playNow, &QPushButton::clicked, this, [this]() {
        emitForSelection(&SemanticSearchDialog::playNowRequested);
    });
    connect(m_playNext, &QPushButton::clicked, this, [this]() {
        emitForSelection(&SemanticSearchDialog::playNextRequested);
    });
    connect(m_enqueue, &QPushButton::clicked, this, [this]() {
        emitForSelection(&SemanticSearchDialog::addToQueueRequested);
    });
}

SemanticSearchDialog::~SemanticSearchDialog()
{
    m_generation.fetch_add(1); // orphan any in-flight worker result
    joinWorker();
}

void SemanticSearchDialog::focusQuery()
{
    m_query->setFocus(Qt::ShortcutFocusReason);
    m_query->selectAll();
}

void SemanticSearchDialog::joinWorker()
{
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void SemanticSearchDialog::submitQuery()
{
    const QString text = m_query->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    joinWorker();
    const quint64 generation = m_generation.fetch_add(1) + 1;
    m_query->setEnabled(false);
    m_status->setText(tr("Searching \"%1\"...").arg(text));

    const QString libraryPath = m_libraryDbPath;
    const QString featuresPath = m_featuresDbPath;
    auto embeddingsSnapshot = m_embeddings;
    m_worker = std::thread([this, generation, text, libraryPath, featuresPath, embeddingsSnapshot]() {
        QueryOutcome outcome;
        QElapsedTimer embedTimer;
        embedTimer.start();

        FeatureStore features(featuresPath);
        if (!features.isOpen()) {
            outcome.error = tr("No analyzed features found. Enable semantic analysis and run "
                               "Library > Analyze library audio first.");
            QMetaObject::invokeMethod(this, [this, generation, outcome]() {
                presentOutcome(generation, outcome);
            }, Qt::QueuedConnection);
            return;
        }

        QueryVectorCache::Identity identity;
        if (features.schemaVersion() >= 5) {
            const auto activeGeneration = features.activeSemanticGeneration();
            if (activeGeneration.valid()) {
                identity = {activeGeneration.capability, activeGeneration.model,
                            activeGeneration.checkpointSha256, activeGeneration.featureRevision,
                            activeGeneration.vectorDimension};
            }
        }

        QVector<float> queryVector;
        bool fromCache = false;
        if (identity.valid()) {
            QueryVectorCache cache(QueryVectorCache::defaultPath());
            queryVector = cache.lookup(identity, text);
            fromCache = !queryVector.isEmpty();
        }
        if (queryVector.isEmpty()) {
            const QueryEmbedding::Result embedded = QueryEmbedding::viaFeatures(text, kProviderTimeoutMs);
            if (!embedded.ok()) {
                outcome.error = embedded.error;
                QMetaObject::invokeMethod(this, [this, generation, outcome]() {
                    presentOutcome(generation, outcome);
                }, Qt::QueuedConnection);
                return;
            }
            if (identity.valid()) {
                const QJsonObject metadata = embedded.metadata;
                const bool matches = metadata.value(QStringLiteral("capability")).toString() == identity.capability
                    && metadata.value(QStringLiteral("model")).toString() == identity.model
                    && metadata.value(QStringLiteral("checkpoint_sha256")).toString() == identity.checkpointSha256
                    && metadata.value(QStringLiteral("feature_revision")).toString() == identity.featureRevision
                    && metadata.value(QStringLiteral("dim")).toInt() == identity.vectorDimension;
                if (!matches) {
                    outcome.error = tr("The provider's model no longer matches the analyzed library; "
                                       "run Library > Analyze library audio to refresh embeddings.");
                    QMetaObject::invokeMethod(this, [this, generation, outcome]() {
                        presentOutcome(generation, outcome);
                    }, Qt::QueuedConnection);
                    return;
                }
                QueryVectorCache cache(QueryVectorCache::defaultPath());
                cache.store(identity, text, embedded.vector);
            }
            queryVector = embedded.vector;
        }
        const qint64 embedMs = embedTimer.elapsed();

        QElapsedTimer rankTimer;
        rankTimer.start();
        auto embeddings = embeddingsSnapshot;
        if (!embeddings) {
            const QVector<qint64> groupIds = features.contentGroupIds(1);
            QList<qint64> groupList;
            groupList.reserve(groupIds.size());
            for (qint64 groupId : groupIds) {
                groupList.push_back(groupId);
            }
            embeddings = std::make_shared<QHash<qint64, QVector<float>>>(
                features.embeddingsForGroups(groupList));
        }
        outcome.embeddings = embeddings;
        if (embeddings->isEmpty()) {
            outcome.error = tr("The feature store has no semantic embeddings yet. Enable semantic "
                               "analysis and run Library > Analyze library audio.");
            QMetaObject::invokeMethod(this, [this, generation, outcome]() {
                presentOutcome(generation, outcome);
            }, Qt::QueuedConnection);
            return;
        }

        const QVector<SemanticSearchData::GroupScore> ranked =
            SemanticSearchData::rankEmbeddings(queryVector, *embeddings, kCandidateLimit);

        Database db(QStringLiteral("semantic-search-%1")
                        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!db.open(libraryPath)) {
            outcome.error = tr("Could not open the library database.");
            QMetaObject::invokeMethod(this, [this, generation, outcome]() {
                presentOutcome(generation, outcome);
            }, Qt::QueuedConnection);
            return;
        }
        const QHash<qint64, QString> pins = db.contentGroupPins();
        for (const SemanticSearchData::GroupScore &candidate : ranked) {
            Track track;
            const QString pinned = pins.value(candidate.groupId);
            if (!pinned.isEmpty()) {
                track = db.trackForPath(pinned);
            }
            if (track.path.isEmpty()) {
                const QStringList paths = features.pathsInGroup(candidate.groupId);
                for (const QString &path : paths) {
                    track = db.trackForPath(path);
                    if (!track.path.isEmpty()) {
                        break;
                    }
                }
            }
            if (track.path.isEmpty()) {
                continue;
            }
            outcome.rows.push_back({track, candidate.groupId, candidate.score});
            if (outcome.rows.size() >= kResultLimit) {
                break;
            }
        }
        outcome.statusDetail = tr("%1 matches from %2 embeddings. Query %3 ms (%4), ranking %5 ms.")
                                   .arg(outcome.rows.size())
                                   .arg(QLocale().toString(static_cast<qlonglong>(embeddings->size())))
                                   .arg(embedMs)
                                   .arg(fromCache ? tr("cached") : tr("provider"))
                                   .arg(rankTimer.elapsed());

        QMetaObject::invokeMethod(this, [this, generation, outcome]() {
            presentOutcome(generation, outcome);
        }, Qt::QueuedConnection);
    });
}

void SemanticSearchDialog::presentOutcome(quint64 generation, QueryOutcome outcome)
{
    if (generation != m_generation.load()) {
        return; // superseded or closing
    }
    m_query->setEnabled(true);
    m_query->setFocus();
    if (outcome.embeddings) {
        m_embeddings = outcome.embeddings;
    }
    if (!outcome.error.isEmpty()) {
        m_status->setText(outcome.error);
        return;
    }

    m_rows = outcome.rows;
    m_results->clear();
    int rank = 0;
    for (const ResultRow &row : m_rows) {
        auto *item = new QTreeWidgetItem(m_results);
        ++rank;
        item->setText(ColRank, QString::number(rank));
        item->setText(ColScore, QString::number(row.score, 'f', 3));
        item->setTextAlignment(ColScore, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(ColTitle, row.track.title.isEmpty() ? row.track.filename : row.track.title);
        item->setText(ColArtist, row.track.artistName);
        item->setText(ColAlbum, row.track.albumTitle);
        item->setText(ColYear, SemanticSearchData::yearText(row.track.date));
        item->setText(ColLength, humanquantity::formatDuration(row.track.durationMs));
        item->setTextAlignment(ColLength, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(ColFormat, SemanticSearchData::formatQuality(row.track));
        item->setText(ColRating, SemanticSearchData::starText(row.track.effectiveRating0To100));
        item->setToolTip(ColTitle, row.track.path);
    }
    for (int column : {ColRank, ColScore, ColYear, ColLength, ColFormat, ColRating}) {
        m_results->resizeColumnToContents(column);
    }
    m_status->setText(outcome.statusDetail);
    if (!m_rows.isEmpty()) {
        m_results->setCurrentItem(m_results->topLevelItem(0));
    }
}

QVector<Track> SemanticSearchDialog::selectedTracks() const
{
    QVector<Track> tracks;
    const QList<QTreeWidgetItem *> selected = m_results->selectedItems();
    tracks.reserve(selected.size());
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_results->topLevelItem(i);
        if (item->isSelected() && i < m_rows.size()) {
            tracks.push_back(m_rows.at(i).track);
        }
    }
    return tracks;
}

void SemanticSearchDialog::emitForSelection(void (SemanticSearchDialog::*signal)(QVector<Track>))
{
    const QVector<Track> tracks = selectedTracks();
    if (!tracks.isEmpty()) {
        emit (this->*signal)(tracks);
    }
}
