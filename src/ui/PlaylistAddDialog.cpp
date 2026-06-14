#include "ui/PlaylistAddDialog.h"

#include "search/Exclusion.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"
#include "search/SearchWorker.h"
#include "ui/SearchResultDelegate.h"
#include "ui/SearchResultsModel.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QFrame>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

Track trackFromRecord(const Search::SearchRecord &rec)
{
    Track t;
    t.path = rec.path;
    t.filename = rec.filename;
    t.title = rec.title;
    t.artistName = rec.artistName;
    t.albumArtistName = rec.albumArtistName;
    t.albumTitle = rec.albumTitle;
    t.date = rec.date;
    t.durationMs = rec.durationMs;
    t.effectiveRating0To100 = rec.rating0To100;
    t.rating0To100 = rec.rating0To100;
    t.sampleRateHz = rec.sampleRateHz;
    t.bitrateKbps = rec.bitrateKbps;
    t.channels = rec.channels;
    t.codec = rec.codec;
    return t;
}

} // namespace

PlaylistAddDialog::PlaylistAddDialog(const QString &dbPath, const QString &playlistName, QWidget *parent)
    : QDialog(parent)
    , m_playlistName(playlistName)
{
    setWindowTitle(QStringLiteral("Add to \"%1\"").arg(playlistName));
    setModal(true);
    resize(620, 560);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_box = new QLineEdit(this);
    m_box->setPlaceholderText(QStringLiteral("Search to add…  RET adds & readies the next, C-/ undo, Esc closes"));
    m_box->setClearButtonEnabled(true);
    m_box->setContentsMargins(8, 6, 8, 4);
    layout->addWidget(m_box);

    m_status = new QLabel(this);
    m_status->setContentsMargins(8, 0, 8, 4);
    layout->addWidget(m_status);

    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    m_model = new SearchResultsModel(this);
    m_delegate = new SearchResultDelegate(this);
    m_list = new QListView(this);
    m_list->setModel(m_model);
    m_list->setItemDelegate(m_delegate);
    m_list->setUniformItemSizes(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);  // keep keyboard focus in the box
    layout->addWidget(m_list, 1);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(20);
    connect(m_debounce, &QTimer::timeout, this, &PlaylistAddDialog::onDebounceTimeout);
    connect(m_box, &QLineEdit::textChanged, this, [this]() { m_debounce->start(); });

    // Ranking tuned for adding: relevance first, then keep an album's tracks
    // together, then prefer higher quality — so picking the right take is easy.
    m_rankConfig.rules = {
        {Search::RankKind::Relevance, {}, {}, {}, true},
        {Search::RankKind::LibraryOrder, MusicSort::SortField::AlbumTitle, {},
         MusicSort::SortDirection::Ascending, true},
        {Search::RankKind::AudioQuality, {}, {}, {}, true},
    };
    m_ranker.setConfig(m_rankConfig);

    m_box->installEventFilter(this);

    qRegisterMetaType<QVector<Search::ExcludeRule>>();
    m_workerThread = new QThread(this);
    m_worker = new Search::SearchWorker(dbPath);
    m_worker->moveToThread(m_workerThread);
    // The index streams in (cold) or loads from cache then refreshes in the
    // background; re-query on each so results stay current.
    connect(m_worker, &Search::SearchWorker::indexGrew, this, &PlaylistAddDialog::onIndexReady, Qt::QueuedConnection);
    connect(m_worker, &Search::SearchWorker::indexLoaded, this, &PlaylistAddDialog::onIndexReady, Qt::QueuedConnection);
    connect(m_worker, &Search::SearchWorker::indexRefreshed, this, &PlaylistAddDialog::onIndexReady, Qt::QueuedConnection);
    connect(m_worker, &Search::SearchWorker::indexError, this, &PlaylistAddDialog::onIndexError, Qt::QueuedConnection);
    connect(m_worker, &Search::SearchWorker::resultsReady, this, &PlaylistAddDialog::onResultsReady, Qt::QueuedConnection);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread->start();

    updateStatus();
}

PlaylistAddDialog::~PlaylistAddDialog()
{
    if (m_workerThread != nullptr) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
        m_workerThread = nullptr;
        m_worker = nullptr;  // deleteLater on thread finish
    }
}

void PlaylistAddDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!m_indexLoaded && m_worker != nullptr) {
        m_status->setText(QStringLiteral("Loading index…"));
        QMetaObject::invokeMethod(m_worker, "buildIndex", Qt::QueuedConnection);
    }
    m_box->setFocus();
}

void PlaylistAddDialog::setAddedPaths(const QSet<QString> &paths)
{
    m_delegate->setAddedPaths(paths);
    m_list->viewport()->update();
}

void PlaylistAddDialog::setQueryText(const QString &query)
{
    m_box->setText(query);
    m_box->selectAll();
    m_box->setFocus();
}

void PlaylistAddDialog::onDebounceTimeout()
{
    submitQuery();
}

void PlaylistAddDialog::submitQuery()
{
    if (m_worker == nullptr || !m_indexLoaded) {
        return;
    }
    const QString text = m_box->text().trimmed();
    if (text.isEmpty()) {
        m_matchCount = 0;
        m_delegate->setQuery(Search::SearchQuery{}, false);
        m_model->clear();
        updateStatus();
        return;
    }
    m_delegate->setQuery(Search::SearchQuery::parse(text), false);
    ++m_queryId;
    QMetaObject::invokeMethod(m_worker, "runQuery", Qt::QueuedConnection,
                              Q_ARG(quint64, m_queryId), Q_ARG(QString, text), Q_ARG(bool, false));
}

void PlaylistAddDialog::onIndexReady(int count)
{
    m_indexLoaded = true;
    m_totalIndexed = count;
    updateStatus();
    submitQuery();
}

void PlaylistAddDialog::onIndexError(const QString &error)
{
    m_status->setText(QStringLiteral("Index error: %1").arg(error));
}

void PlaylistAddDialog::setPreferredPaths(const QStringList &paths)
{
    m_preferredPaths = QSet<QString>(paths.cbegin(), paths.cend());
}

void PlaylistAddDialog::onResultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int totalMatches)
{
    if (queryId != m_queryId) {
        return;
    }
    m_matchCount = totalMatches;
    m_ranker.sort(results);
    if (!m_preferredPaths.isEmpty()) {
        // Keep ranker order within each group, candidates first.
        std::stable_partition(results.begin(), results.end(), [this](const Search::ScoredResult &result) {
            return m_preferredPaths.contains(result.rec.path);
        });
    }
    m_model->setResults(std::move(results));
    if (m_model->rowCount() > 0) {
        m_list->scrollToTop();
        setCursorRow(0);
    } else {
        m_delegate->setCurrentRow(-1);
        m_list->viewport()->update();
    }
    updateStatus();
}

void PlaylistAddDialog::updateStatus()
{
    if (!m_indexLoaded) {
        m_status->setText(QStringLiteral("Loading index…"));
        return;
    }
    if (m_box->text().trimmed().isEmpty()) {
        m_status->setText(QStringLiteral("%1 tracks").arg(m_totalIndexed));
        return;
    }
    const int shown = m_model->rowCount();
    const QString countStr = m_matchCount > shown
        ? QStringLiteral("%1 of %2").arg(shown).arg(m_matchCount)
        : QString::number(m_matchCount);
    m_status->setText(QStringLiteral("%1 matches").arg(countStr));
}

void PlaylistAddDialog::moveCursor(int delta)
{
    const int rows = m_model->rowCount();
    if (rows == 0) {
        return;
    }
    const int cur = m_delegate->currentRow();
    const int next = std::clamp((cur < 0 ? 0 : cur) + delta, 0, rows - 1);
    setCursorRow(next);
}

void PlaylistAddDialog::setCursorRow(int row)
{
    m_delegate->setCurrentRow(row);
    const QModelIndex index = m_model->index(row, 0);
    m_list->setCurrentIndex(index);
    m_list->scrollTo(index, QAbstractItemView::EnsureVisible);
    m_list->viewport()->update();
}

void PlaylistAddDialog::chooseCurrent()
{
    const int row = m_delegate->currentRow();
    if (row < 0 || row >= m_model->rowCount()) {
        return;
    }
    const QVariant recVar = m_model->index(row, 0).data(SearchResultsModel::SearchRecordRole);
    if (!recVar.isValid()) {
        return;
    }
    const Track track = trackFromRecord(qvariant_cast<Search::SearchRecord>(recVar));
    if (track.path.isEmpty()) {
        return;
    }
    emit itemChosen(track, m_box->text().trimmed());
    if (m_editMode) {
        accept();
        return;
    }
    // Ready the box for the next add. textChanged clears the results.
    m_box->clear();
    m_box->setFocus();
}

bool PlaylistAddDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_box || event->type() != QEvent::KeyPress) {
        return QDialog::eventFilter(watched, event);
    }
    auto *ke = static_cast<QKeyEvent *>(event);
    const int key = ke->key();
    const bool ctrl = ke->modifiers() & Qt::ControlModifier;

    if (key == Qt::Key_Down || (ctrl && key == Qt::Key_N)) { moveCursor(+1); return true; }
    if (key == Qt::Key_Up   || (ctrl && key == Qt::Key_P)) { moveCursor(-1); return true; }
    if (key == Qt::Key_PageDown) { moveCursor(+8); return true; }
    if (key == Qt::Key_PageUp)   { moveCursor(-8); return true; }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) { chooseCurrent(); return true; }
    if (ctrl && key == Qt::Key_Slash) { emit undoRequested(); return true; }
    if (key == Qt::Key_Escape || (ctrl && key == Qt::Key_G)) {
        if (!m_box->text().isEmpty()) {
            m_box->clear();  // first press clears the query
        } else {
            reject();        // empty box: close the modal
        }
        return true;
    }
    return QDialog::eventFilter(watched, event);
}
