#include "ui/RightSidebar.h"

#include "ui/AlbumArtFallback.h"
#include "ui/DenseTableDelegate.h"

#include <QAction>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QByteArray>
#include <QFormLayout>
#include <QFrame>
#include <QFileInfo>

#include <algorithm>

namespace {

struct ColumnSpec {
    const char *key;
    const char *label;
    int index;
};

constexpr ColumnSpec columns[] = {
    {"position", "#", 0},
    {"title", "Title", 1},
    {"rating", "Rating", 2},
};

QString ratingText(int rating0To100)
{
    if (rating0To100 < 0) {
        return {};
    }
    return QStringLiteral("%1 %2").arg(rating0To100 / 20.0, 0, 'f', 1).arg(QChar(0x2605));
}

QLabel *valueLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

QString formatDuration(qint64 durationMs)
{
    if (durationMs <= 0) {
        return {};
    }

    const qint64 totalSeconds = durationMs / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace

RightSidebar::RightSidebar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter, 1);

    m_queueTable = new QTableWidget(0, 3, m_splitter);
    m_queueTable->setHorizontalHeaderLabels({
        QStringLiteral("#"),
        QStringLiteral("Title"),
        QStringLiteral("Rating"),
    });
    m_queueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queueTable->setItemDelegate(new DenseTableDelegate(this));
    m_queueTable->setShowGrid(false);
    m_queueTable->setWordWrap(false);
    m_queueTable->verticalHeader()->setVisible(false);
    m_queueTable->verticalHeader()->setDefaultSectionSize(20);
    m_queueTable->verticalHeader()->setMinimumSectionSize(20);
    m_queueTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_queueTable->horizontalHeader()->setFixedHeight(20);
    m_queueTable->horizontalHeader()->setSectionsMovable(true);
    m_queueTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_queueTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_queueTable->setAlternatingRowColors(true);
    m_queueTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_queueTable->setStyleSheet(QStringLiteral("QTableWidget::item { padding: 0 3px; }"));
    m_splitter->addWidget(m_queueTable);

    connect(m_queueTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        emit queueTrackActivated(row);
    });
    connect(m_queueTable->horizontalHeader(), &QHeaderView::customContextMenuRequested, this, &RightSidebar::showHeaderMenu);
    connect(m_queueTable->horizontalHeader(), &QHeaderView::sectionMoved, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(m_queueTable->horizontalHeader(), &QHeaderView::sectionResized, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(m_queueTable, &QTableWidget::customContextMenuRequested, this, &RightSidebar::showQueueMenu);

    m_trackInfoPane = new QFrame(m_splitter);
    auto *infoLayout = new QFormLayout(m_trackInfoPane);
    infoLayout->setContentsMargins(6, 6, 6, 6);
    infoLayout->setHorizontalSpacing(8);
    infoLayout->setVerticalSpacing(2);
    m_trackInfoTitle = valueLabel(m_trackInfoPane);
    m_trackInfoArtist = valueLabel(m_trackInfoPane);
    m_trackInfoAlbum = valueLabel(m_trackInfoPane);
    m_trackInfoYear = valueLabel(m_trackInfoPane);
    m_trackInfoFile = valueLabel(m_trackInfoPane);
    m_trackInfoProperties = valueLabel(m_trackInfoPane);
    infoLayout->addRow(QStringLiteral("Title"), m_trackInfoTitle);
    infoLayout->addRow(QStringLiteral("Artist"), m_trackInfoArtist);
    infoLayout->addRow(QStringLiteral("Album"), m_trackInfoAlbum);
    infoLayout->addRow(QStringLiteral("Year"), m_trackInfoYear);
    infoLayout->addRow(QStringLiteral("File"), m_trackInfoFile);
    infoLayout->addRow(QStringLiteral("Properties"), m_trackInfoProperties);
    m_trackInfoPane->setMinimumHeight(96);
    m_splitter->addWidget(m_trackInfoPane);

    m_albumArt = new QLabel(m_splitter);
    m_albumArt->setMinimumSize(180, 180);
    m_albumArt->setMaximumHeight(360);
    m_albumArt->setAlignment(Qt::AlignCenter);
    m_albumArt->setFrameShape(QFrame::StyledPanel);
    m_albumArt->setText(QStringLiteral("Album art"));
    m_albumArt->setScaledContents(false);
    m_splitter->addWidget(m_albumArt);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({420, 150, 240});
    setTrackInfo({});

    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        emit viewSettingsChanged();
    });
}

void RightSidebar::setQueue(const QVector<Track> &tracks)
{
    m_tracks = tracks;
    m_queueTable->setRowCount(0);
    for (int row = 0; row < tracks.size(); ++row) {
        const Track &track = tracks.at(row);
        m_queueTable->insertRow(row);
        m_queueTable->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
        m_queueTable->setItem(row, 1, new QTableWidgetItem(track.title));
        m_queueTable->setItem(row, 2, new QTableWidgetItem(ratingText(track.effectiveRating0To100)));
        m_queueTable->setRowHeight(row, m_rowHeight);
    }
}

void RightSidebar::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_queueTable->rowCount()) {
        m_queueTable->clearSelection();
        return;
    }
    m_queueTable->selectRow(index);
    m_queueTable->scrollToItem(m_queueTable->item(index, 1), QAbstractItemView::PositionAtCenter);
}

void RightSidebar::setAlbumArt(const QString &imagePath)
{
    QPixmap pixmap(imagePath);
    if (pixmap.isNull()) {
        pixmap = QPixmap(AlbumArtFallback::resourcePath(palette()));
        if (pixmap.isNull()) {
            m_albumArt->setPixmap({});
            m_albumArt->setText(QStringLiteral("Album art"));
            return;
        }
    }

    m_albumArt->setText({});
    const int side = std::min(m_albumArt->width(), std::max(180, m_albumArt->height()));
    m_albumArt->setPixmap(pixmap.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void RightSidebar::setTrackInfo(const Track &track)
{
    if (track.path.isEmpty()) {
        m_trackInfoTitle->setText(QStringLiteral("No track playing."));
        m_trackInfoArtist->clear();
        m_trackInfoAlbum->clear();
        m_trackInfoYear->clear();
        m_trackInfoFile->clear();
        m_trackInfoProperties->clear();
        return;
    }

    m_trackInfoTitle->setText(track.title.isEmpty() ? track.filename : track.title);
    m_trackInfoArtist->setText(track.artistName);
    m_trackInfoAlbum->setText(track.albumTitle);
    m_trackInfoYear->setText(track.date.left(4));
    m_trackInfoFile->setText(track.path);

    QStringList properties;
    const QString duration = formatDuration(track.durationMs);
    if (!duration.isEmpty()) {
        properties.push_back(duration);
    }
    if (track.discNumber > 0) {
        properties.push_back(QStringLiteral("Disc %1").arg(track.discNumber));
    }
    if (track.trackNumber > 0) {
        properties.push_back(QStringLiteral("Track %1").arg(track.trackNumber));
    }
    m_trackInfoProperties->setText(properties.join(QStringLiteral(" / ")));
}

void RightSidebar::setTrackInfoVisible(bool visible)
{
    m_trackInfoPane->setVisible(visible);
}

QString RightSidebar::viewSettingsJson() const
{
    QJsonArray visibleColumns;
    for (const ColumnSpec &spec : columns) {
        if (!m_queueTable->isColumnHidden(spec.index)) {
            visibleColumns.append(QString::fromLatin1(spec.key));
        }
    }

    QJsonObject root;
    root.insert(QStringLiteral("visibleColumns"), visibleColumns);
    root.insert(QStringLiteral("headerHeight"), m_queueTable->horizontalHeader()->height());
    root.insert(QStringLiteral("rowHeight"), m_queueTable->verticalHeader()->defaultSectionSize());
    root.insert(QStringLiteral("headerState"), QString::fromLatin1(m_queueTable->horizontalHeader()->saveState().toBase64()));
    root.insert(QStringLiteral("showTrackInfo"), m_trackInfoPane->isVisible());
    QJsonArray splitterSizes;
    for (int size : m_splitter->sizes()) {
        splitterSizes.append(size);
    }
    root.insert(QStringLiteral("splitter"), splitterSizes);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void RightSidebar::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray visible = root.value(QStringLiteral("visibleColumns")).toArray();
    if (!visible.isEmpty()) {
        QStringList visibleKeys;
        for (const QJsonValue &value : visible) {
            visibleKeys.push_back(value.toString());
        }
        for (const ColumnSpec &spec : columns) {
            m_queueTable->setColumnHidden(spec.index, !visibleKeys.contains(QString::fromLatin1(spec.key)));
        }
    }

    setHeaderHeight(root.value(QStringLiteral("headerHeight")).toInt(20));
    const int rowHeight = root.value(QStringLiteral("rowHeight")).toInt(20);
    m_rowHeight = rowHeight <= 24 ? 20 : std::clamp(rowHeight, 18, 48);
    m_queueTable->verticalHeader()->setDefaultSectionSize(m_rowHeight);
    const QByteArray headerState = QByteArray::fromBase64(root.value(QStringLiteral("headerState")).toString().toLatin1());
    if (!headerState.isEmpty()) {
        m_queueTable->horizontalHeader()->restoreState(headerState);
    }
    const QJsonArray splitter = root.value(QStringLiteral("splitter")).toArray();
    QList<int> sizes;
    for (const QJsonValue &value : splitter) {
        sizes.push_back(value.toInt());
    }
    if (sizes.size() == 2) {
        m_splitter->setSizes({sizes.value(0), 150, sizes.value(1)});
    } else if (sizes.size() == 3) {
        m_splitter->setSizes(sizes);
    }
    setTrackInfoVisible(root.value(QStringLiteral("showTrackInfo")).toBool(true));
}

void RightSidebar::setHeaderHeight(int height)
{
    m_queueTable->horizontalHeader()->setFixedHeight(std::clamp(height, 18, 40));
}

void RightSidebar::showHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    for (const ColumnSpec &spec : columns) {
        QAction *action = menu.addAction(QString::fromLatin1(spec.label));
        action->setCheckable(true);
        action->setChecked(!m_queueTable->isColumnHidden(spec.index));
        connect(action, &QAction::toggled, this, [this, column = spec.index](bool checked) {
            m_queueTable->setColumnHidden(column, !checked);
            emit viewSettingsChanged();
        });
    }
    menu.exec(m_queueTable->horizontalHeader()->mapToGlobal(pos));
}

void RightSidebar::showQueueMenu(const QPoint &pos)
{
    const int row = m_queueTable->rowAt(pos.y());
    if (row < 0 || row >= m_tracks.size()) {
        return;
    }

    const Track track = m_tracks.at(row);
    QMenu menu(this);
    QAction *findFile = menu.addAction(QStringLiteral("Find file"));
    connect(findFile, &QAction::triggered, this, [this, track]() {
        emit findFileRequested(track);
    });
    menu.exec(m_queueTable->viewport()->mapToGlobal(pos));
}
