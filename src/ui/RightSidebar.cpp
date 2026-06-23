#include "ui/RightSidebar.h"

#include "db/Database.h"
#include "ui/AlbumArtFallback.h"
#include "ui/AlbumArtView.h"
#include "ui/QueueStore.h"
#include "ui/SplitterPersistence.h"
#include "ui/trackinfo/TrackInfoPanel.h"

#include <QEvent>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace {
// Per-pane height floors for the vertical splitter (queue / track info / album
// art), plus a floor for the total. Distributions below these are degenerate
// (one pane collapsed to ~0, or the panel laid out before it has a real height)
// and must never be persisted or restored. See SplitterPersistence.h.
constexpr int kQueuePaneMinimumHeight = 60;
constexpr int kTrackInfoPaneMinimumHeight = 48;
constexpr int kAlbumArtPaneMinimumHeight = 120;
constexpr int kRightSidebarSplitterMinimumTotal = 300;
const QList<int> kRightSidebarPaneMinimums = {kQueuePaneMinimumHeight,
                                              kTrackInfoPaneMinimumHeight,
                                              kAlbumArtPaneMinimumHeight};
const QList<int> kRightSidebarDefaultSizes = {519, 152, 286};
}  // namespace


RightSidebar::RightSidebar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter, 1);

    m_queueTable = new QueueTable(QueueTablePreset::Sidebar, m_splitter);
    m_queueTable->setTableBorders(panelBorders(1, 1, 0, 1));
    m_splitter->addWidget(m_queueTable);

    connect(m_queueTable, &QueueTable::trackActivated, this, &RightSidebar::queueTrackActivated);
    connect(m_queueTable, &QueueTable::trackRatingChanged, this, &RightSidebar::queueTrackRatingChanged);
    connect(m_queueTable, &QueueTable::rowsMoveRequested, this, &RightSidebar::queueRowsMoveRequested);
    connect(m_queueTable, &QueueTable::rowsRemoveRequested, this, &RightSidebar::queueRowsRemoveRequested);
    connect(m_queueTable, &QueueTable::removeAllMissingTracksRequested, this, &RightSidebar::removeAllMissingTracksRequested);
    connect(m_queueTable, &QueueTable::clearRequested, this, &RightSidebar::queueClearRequested);
    connect(m_queueTable, &QueueTable::clearPlayNextPriorityRequested, this, &RightSidebar::clearPlayNextPriorityRequested);
    connect(m_queueTable, &QueueTable::saveQueueAsRequested, this, &RightSidebar::saveQueueAsRequested);
    connect(m_queueTable, &QueueTable::restorePreviousQueueRequested, this, &RightSidebar::restorePreviousQueueRequested);
    connect(m_queueTable, &QueueTable::unlinkFromPlaylistRequested, this, &RightSidebar::unlinkQueueFromPlaylistRequested);
    connect(m_queueTable, &QueueTable::findFileRequested, this, &RightSidebar::findFileRequested);
    connect(m_queueTable, &QueueTable::propertiesRequested, this, &RightSidebar::propertiesRequested);
    connect(m_queueTable, &QueueTable::trackLibraryRequested, this, &RightSidebar::trackLibraryRequested);
    connect(m_queueTable, &QueueTable::viewSettingsChanged, this, &RightSidebar::viewSettingsChanged);

    m_trackInfoPanel = new TrackInfoPanel(m_splitter);
    connect(m_trackInfoPanel, &TrackInfoPanel::artistRequested, this, &RightSidebar::artistRequested);
    connect(m_trackInfoPanel, &TrackInfoPanel::albumRequested, this, &RightSidebar::albumRequested);
    connect(m_trackInfoPanel, &TrackInfoPanel::findFileRequested, this, &RightSidebar::findFileRequested);
    connect(m_trackInfoPanel, &TrackInfoPanel::settingsChanged, this, &RightSidebar::viewSettingsChanged);
    m_splitter->addWidget(m_trackInfoPanel);

    m_albumArt = new AlbumArtView(m_splitter);
    m_albumArt->setMinimumSize(180, 180);
    m_albumArt->setText(QStringLiteral("Album art"));
    m_splitter->addWidget(m_albumArt);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes(kRightSidebarDefaultSizes);
    m_userSplitterSizes = kRightSidebarDefaultSizes;
    setTrackInfo({});

    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        // Only a real drag updates the persisted sizes, and only when the live
        // distribution is sane: programmatic setSizes() emits no splitterMoved,
        // and a transient layout (tiny window, hidden track-info pane) must not
        // clobber the user-tuned three-pane sizes.
        const QList<int> live = m_splitter->sizes();
        if (SplitterPersistence::splitterSizesAreStable(live, kRightSidebarPaneMinimums,
                                                        kRightSidebarSplitterMinimumTotal)) {
            m_userSplitterSizes = live;
        }
        emit viewSettingsChanged();
    });
}

void RightSidebar::setQueueStore(QueueStore *store)
{
    m_queueStore = store;
    m_queueTable->setQueueStore(store);
}

void RightSidebar::setQueueIsPlaylistSourced(bool sourced)
{
    m_queueTable->setQueueIsPlaylistSourced(sourced);
}

void RightSidebar::setQueue(const QVector<Track> &tracks)
{
    const int priorRow = queueCurrentRow();
    m_tracks = tracks;
    if (m_queueStore != nullptr) {
        m_queueStore->setTracks(tracks);
    }
    if (!tracks.isEmpty()) {
        const int row = priorRow >= 0 && priorRow < tracks.size()
            ? priorRow
            : (m_currentQueueIndex >= 0 && m_currentQueueIndex < tracks.size() ? m_currentQueueIndex : 0);
        setQueueCurrentRow(row);
    }
}

void RightSidebar::setPlayNextRange(int begin, int end)
{
    m_playNextBegin = begin;
    m_playNextEnd = end;
    if (m_queueStore != nullptr) {
        m_queueStore->setPlayNextRange(begin, end);
    }
}

void RightSidebar::setCurrentIndex(int index, bool reveal)
{
    // The "currently playing" row is shown with its own indicator and is kept
    // independent of the user's selection, so reordering/adding/removing never
    // hijacks the selection or scrolls the view. Only explicit playback changes
    // (reveal == true) scroll the playing row into view.
    const int rowCount = queueRowCount();
    const int current = (index >= 0 && index < rowCount) ? index : -1;
    m_currentQueueIndex = current;
    if (m_queueStore != nullptr) {
        m_queueStore->setCurrentIndex(current);
    }

    if (reveal && current >= 0) {
        m_queueTable->revealCurrentPlaying();
    }
}

void RightSidebar::setNavigationScrollPadding(int rows)
{
    if (m_queueTable != nullptr) {
        m_queueTable->setNavigationScrollPadding(rows);
    }
}

QWidget *RightSidebar::queueNavigationWidget() const
{
    return m_queueTable == nullptr ? nullptr : m_queueTable->navigationWidget();
}

int RightSidebar::queueRowCount() const
{
    return m_queueTable != nullptr ? m_queueTable->rowCount() : 0;
}

int RightSidebar::queueCurrentRow() const
{
    return m_queueTable != nullptr ? m_queueTable->currentRow() : -1;
}

void RightSidebar::setQueueCurrentRow(int row)
{
    setQueueCurrentRow(row, 0);
}

void RightSidebar::setQueueCurrentRow(int row, int scrollDirection)
{
    if (m_queueTable != nullptr) {
        m_queueTable->setCurrentRow(row, scrollDirection);
    }
}

void RightSidebar::moveQueueCurrentRow(int delta)
{
    if (queueRowCount() == 0) {
        return;
    }
    const int row = queueCurrentRow() >= 0 ? queueCurrentRow() : 0;
    setQueueCurrentRow(std::clamp(row + delta, 0, queueRowCount() - 1), delta);
}

void RightSidebar::activateCurrentQueueTrack()
{
    const int row = queueCurrentRow();
    if (row >= 0 && row < queueRowCount()) {
        m_queueTable->activateCurrentRow();
    }
}

QVector<Search::MatchDocument> RightSidebar::queueSearchDocuments() const
{
    return m_queueStore == nullptr ? QVector<Search::MatchDocument>() : m_queueStore->searchDocuments();
}

void RightSidebar::setAlbumArt(const QString &imagePath)
{
    const bool valid = !imagePath.isEmpty() && QFileInfo::exists(imagePath);
    const QString effectivePath = valid ? imagePath : AlbumArtFallback::resourcePath(palette());
    m_usingArtFallback = !valid;

    if (effectivePath.isEmpty()) {
        m_albumArt->setPixmap({});
        m_albumArt->setText(QStringLiteral("Album art"));
        return;
    }

    m_albumArt->setText({});
    static_cast<AlbumArtView *>(m_albumArt)->setSourcePath(effectivePath);
}

void RightSidebar::setAlbumArt(const QImage &image)
{
    if (image.isNull()) {
        setAlbumArt(QString());
        return;
    }
    m_usingArtFallback = false;
    m_albumArt->setText({});
    static_cast<AlbumArtView *>(m_albumArt)->setSourceImage(image);
}

void RightSidebar::setTrackInfo(const Track &track)
{
    // Queue loaders skip the rich audio columns (sampleRateHz, bitDepth, channels,
    // bitrateKbps, codec) for speed.  Fill them now so the metadata row can show
    // these values; it's one indexed SQL read per track change — negligible cost.
    if (m_database != nullptr && !track.path.isEmpty()) {
        Track rich = track;
        m_database->enrichTrackForStatus(rich);
        m_trackInfoPanel->setTrack(rich);
        return;
    }
    m_trackInfoPanel->setTrack(track);
}

void RightSidebar::setTrackInfoVisible(bool visible)
{
    const bool wasHidden = m_trackInfoPanel->isHidden();
    m_trackInfoPanel->setVisible(visible);
    // Re-showing a hidden pane: QSplitter would otherwise hand it an arbitrary
    // slice. Restore the last user-tuned three-pane distribution so the queue /
    // track-info / album-art boundaries land where the user left them.
    if (visible && wasHidden
        && SplitterPersistence::splitterSizesAreStable(m_userSplitterSizes, kRightSidebarPaneMinimums,
                                                       kRightSidebarSplitterMinimumTotal)) {
        m_splitter->setSizes(m_userSplitterSizes);
    }
}

void RightSidebar::configureTrackInfoPanel(QWidget *parent)
{
    m_trackInfoPanel->configure(parent);
}

QString RightSidebar::viewSettingsJson() const
{
    QJsonObject root = QJsonDocument::fromJson(m_queueTable->viewSettingsJson().toUtf8()).object();
    root.insert(QStringLiteral("showTrackInfo"), !m_trackInfoPanel->isHidden());
    const QJsonObject info = m_trackInfoPanel->settings();
    for (auto it = info.begin(); it != info.end(); ++it) {
        root.insert(it.key(), it.value());
    }
    // Persist the last user-tuned sizes, never the live ones: a save can be
    // triggered (track-info/queue settings changes) while the splitter is in a
    // transient or track-info-hidden distribution, which would corrupt the layout.
    root.insert(QStringLiteral("splitter"), SplitterPersistence::splitterSizesToJson(m_userSplitterSizes));
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void RightSidebar::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    m_queueTable->applyViewSettingsJson(json);
    QList<int> sizes = SplitterPersistence::splitterSizesFromJson(root.value(QStringLiteral("splitter")).toArray());
    if (sizes.size() == 2) {
        // Legacy two-pane layout (queue + album art) from before the track-info
        // pane existed; insert a default middle size so it restores as three.
        sizes = {sizes.value(0), 150, sizes.value(1)};
    } else if (sizes.size() == 3 && sizes.at(1) < kTrackInfoPaneMinimumHeight) {
        // A layout saved while the track-info pane was hidden records its size as
        // ~0. Substitute the default so the stability check passes and the queue /
        // album-art ratio survives instead of resetting to the built-in defaults.
        sizes[1] = 150;
    }
    // Restore only a sane distribution; an unstable/missing one leaves the
    // constructor defaults in place. Keep the persisted sizes in sync so a later
    // save doesn't overwrite them with the defaults.
    if (SplitterPersistence::restoreSplitterIfStable(m_splitter,
            SplitterPersistence::splitterSizesToJson(sizes),
            kRightSidebarPaneMinimums, kRightSidebarSplitterMinimumTotal)) {
        m_userSplitterSizes = sizes;
    }
    setTrackInfoVisible(root.value(QStringLiteral("showTrackInfo")).toBool(true));
    m_trackInfoPanel->applySettings(root);
}

void RightSidebar::resetViewSettings()
{
    m_queueTable->resetViewSettings();
    m_splitter->setSizes(kRightSidebarDefaultSizes);
    m_userSplitterSizes = kRightSidebarDefaultSizes;
    setTrackInfoVisible(true);
    m_trackInfoPanel->resetToDefaults();
    emit viewSettingsChanged();
}

void RightSidebar::setHeaderHeight(int height)
{
    QJsonObject root = QJsonDocument::fromJson(m_queueTable->viewSettingsJson().toUtf8()).object();
    root.insert(QStringLiteral("headerHeight"), std::clamp(height, 18, 40));
    m_queueTable->applyViewSettingsJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void RightSidebar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        m_queueTable->update();
    }
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) && m_usingArtFallback) {
        setAlbumArt(QString());
    }
}
