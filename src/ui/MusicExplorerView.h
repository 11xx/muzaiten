#pragma once

#include "core/Album.h"
#include "core/MusicSort.h"
#include "core/Track.h"
#include "search/SearchMatcher.h"
#include "ui/TrackTable.h"

#include <QHash>
#include <QImage>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class ArtworkCache;
class QColor;
class QScrollArea;
class QVBoxLayout;

class MusicExplorerView final : public QWidget {
    Q_OBJECT

public:
    explicit MusicExplorerView(QWidget *parent = nullptr);

    void setArtworkCache(ArtworkCache *cache);
    void setAlbums(const QVector<Album> &albums);
    void setTrackProvider(std::function<QVector<Track>(const Album &album)> provider);
    void setQueueIsPlaylistSourced(bool sourced);
    void refreshExpandedTracks();
    void applyAlbumGridViewSettingsJson(const QString &json);
    void applyTrackTableViewSettingsJson(const QString &json);
    QString trackTableViewSettingsJson() const;
    void setNavigationScrollPadding(int rows);
    QWidget *albumNavigationWidget() { return this; }
    TrackTable *trackNavigationWidget() const { return m_inlineTrackTable; }
    int rowCount() const;
    int currentRow() const { return m_currentAlbumRow; }
    void setCurrentRow(int row);
    void moveCurrentByGrid(int horizontal, int vertical);
    void expandCurrentAlbum(bool focusTracks = false);
    void collapseExpandedAlbum();
    void activateCurrentAlbum();
    QString currentAlbumTitle() const;
    QStringList albumTitlesForAction() const;
    void addCurrentAlbumToQueue();
    void addCurrentAlbumToPlaylist();
    void playNextCurrentAlbum();
    void selectAlbumTitle(const QString &albumTitle, bool focusTracks = false);
    void selectTrackByPath(const QString &path);
    int trackRowCount() const;
    int currentTrackRow() const;
    void setCurrentTrackRow(int row);
    void setCurrentTrackRow(int row, int scrollDirection);
    QString expandedAlbumTitle() const { return m_expandedAlbumTitle; }
    int expandedPanelCountForTests() const;
    int pointerXForTests() const;
    int columnCountForTests() const { return m_columnCount; }
    int scrollValueForTests() const;
    QRect cardGeometryForTests(int row) const;
    QWidget *cardWidgetForTests(int row) const;
    QVector<Search::MatchDocument> albumSearchDocuments() const;
    QVector<Search::MatchDocument> trackSearchDocuments() const;
    TrackTable *inlineTrackTableForTests() const { return m_inlineTrackTable; }

signals:
    void albumPlayNextRequested(const QString &albumTitle);
    void albumPlayReplaceRequested(const QStringList &albumTitles);
    void albumAddToQueueRequested(const QString &albumTitle);
    void albumPlayNextTemporaryRequested(const QString &albumTitle);
    void albumAddToQueueTemporaryRequested(const QString &albumTitle);
    void albumAddToPlaylistRequested(const QStringList &albumTitles);
    void albumRatingChanged(const QString &albumArtistName, const QString &albumTitle, int rating0To100);
    void trackActivated(const Track &track);
    void trackPlayNextRequested(const QVector<Track> &tracks);
    void trackAddToQueueRequested(const QVector<Track> &tracks);
    void trackPlayNextTemporaryRequested(const QVector<Track> &tracks);
    void trackAddToQueueTemporaryRequested(const QVector<Track> &tracks);
    void trackAddToPlaylistRequested(const QVector<Track> &tracks);
    void findFileRequested(const Track &track);
    void propertiesRequested(const Track &track);
    void trackRatingChanged(const Track &track, int rating0To100);
    void trackTableViewSettingsChanged();

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    class AlbumCard;
    class ExpandedPanel;

    void rebuildLayout();
    void rebuildLayoutLater();
    bool placeExpandedPanelAtRow(int row);
    bool removeExpandedPanelInPlace();
    bool recomputeEffectiveMetrics();
    void applySortedAlbums(const QVector<Album> &albums);
    void clearContent();
    void requestVisibleArtwork();
    void requestArtworkForAlbum(int row);
    void setCurrentRowInternal(int row, bool ensureVisible);
    void setExpandedAlbumRow(int row, bool focusTracks);
    void clearExpandedAlbum();
    void updateCardSelection();
    void refreshActiveHighlight();
    void updateExpandedPanelGeometry();
    void refreshExpandedPanelBackdrop();
    void applyExpandedTrackPalette(const QColor &tint);
    void ensureExpandedPanelVisible();
    void ensureInlineTrackVisible(int direction);
    void showAlbumContextMenu(int row, const QPoint &globalPos);
    bool handleInlineTrackKey(QKeyEvent *event);
    int columnCountForWidth(int width) const;
    int rowForTitle(const QString &albumTitle) const;
    QRect cardGeometryInContent(int row) const;
    QColor artTintForAlbum(const QString &albumTitle) const;
    int availableGridWidth() const;

    ArtworkCache *m_artworkCache = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_rows = nullptr;
    ExpandedPanel *m_expandedPanel = nullptr;
    TrackTable *m_inlineTrackTable = nullptr;
    QVector<Album> m_sourceAlbums;
    QVector<Album> m_albums;
    QVector<AlbumCard *> m_cards;
    QVector<QWidget *> m_gridRows;
    QHash<QString, QImage> m_artByTitle;
    std::function<QVector<Track>(const Album &album)> m_trackProvider;
    int m_currentAlbumRow = -1;
    int m_expandedAlbumRow = -1;
    int m_columnCount = 1;
    int m_artworkGeneration = 0;
    QString m_expandedAlbumTitle;
    bool m_queueIsPlaylistSourced = false;
    bool m_rebuildQueued = false;
    bool m_rebuildingLayout = false;
    int m_cellWidth = 204;
    int m_cellHeight = 292;
    int m_artSize = 176;
    int m_effectiveCellWidth = 204;
    int m_effectiveCellHeight = 292;
    int m_effectiveArtSize = 176;
    int m_spacing = 6;
    int m_starSize = 18;
    Qt::Alignment m_textAlignment = Qt::AlignHCenter;
    MusicSort::SortField m_sortField = MusicSort::SortField::Year;
    bool m_sortDescending = true;
    bool m_sortReverseGroups = false;
};
