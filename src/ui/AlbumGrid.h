#pragma once

#include "core/Album.h"
#include "core/MusicSort.h"
#include "search/SearchMatcher.h"

#include <QListView>
#include <QSet>
#include <QVector>

class ArtworkCache;
class QEvent;
class QImage;
class QMouseEvent;
class QRubberBand;
class QStandardItem;
class QTimer;
class QWheelEvent;

class AlbumGrid final : public QListView {
    Q_OBJECT

public:
    explicit AlbumGrid(QWidget *parent = nullptr);

    void setArtworkCache(ArtworkCache *cache);
    // freshLoad = true when loading a different artist's albums; it shows a
    // per-album loading spinner while art is fetched. Within-artist selection
    // changes and narrow rating refreshes pass false (no spinner).
    void setAlbums(const QVector<Album> &albums, bool freshLoad = false);
    void setSelectedAlbumTitle(const QString &albumTitle);
    void setRememberedOutlineVisible(bool visible);
    int loadingAngle() const { return m_loadingAngle; }
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    int rowCount() const;
    int currentRow() const;
    void setCurrentRow(int row);
    void moveCurrentByGrid(int horizontal, int vertical);
    void activateCurrentAlbum();
    void addCurrentAlbumToQueue();
    void addCurrentAlbumToPlaylist();
    void playNextCurrentAlbum();
    void markCurrentAlbum();
    void markAllAlbums();
    void unmarkCurrentAlbum();
    void unmarkAllAlbums();
    QString currentAlbumTitle() const;
    QStringList albumTitlesForAction() const;
    QVector<Search::MatchDocument> searchDocuments() const;

signals:
    void albumSelectionToggled(const QString &albumTitle);
    void albumSelectionCleared();
    void albumSelectionNarrowRequested(const QStringList &albumTitles);
    // Lightweight "narrowing follows selection" — emitted as the cursor moves or
    // the marked set changes so the track table re-narrows live without the full
    // grid rebuild that albumSelectionNarrowRequested (n key) triggers.
    void albumNarrowFollowRequested(const QStringList &albumTitles);
    void albumAddToPlaylistRequested(const QStringList &albumTitles);
    void albumPlayNextRequested(const QString &albumTitle);
    void albumPlayReplaceRequested(const QStringList &albumTitles);
    void albumAddToQueueRequested(const QString &albumTitle);
    void albumRatingChanged(const QString &albumArtistName, const QString &albumTitle, int rating0To100);
    void viewSettingsChanged();

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QRect ratingRectForIndex(const QModelIndex &index) const;
    int gridColumnCount() const;
    void showContextMenu(const QPoint &pos);
    // Recomputes the effective cell/art sizes so a whole number of columns at the
    // configured base width fills the viewport, distributing the slack instead of
    // leaving it as right-edge whitespace (MusicBee-style).
    void recomputeEffectiveSizes();
    void applySettingsToView();
    void applySettingsToItems();
    void applySort();
    void populateItemFromAlbum(class QStandardItem *item, const Album &album);
    void appendNextAlbumBatch();
    void loadNextAlbumArtwork();
    void onArtworkReady(const QString &token, const QImage &image, quint64 generation);
    void onArtworkMissing(const QString &token, quint64 generation);
    void clearItemLoading(int row);
    void reselectMarkedAlbums();
    // Emits albumNarrowFollowRequested for the current action set (the marked
    // albums, or the lone current album when nothing is marked).
    void followNarrowToSelection();
    void setCurrentAlbumMarked(bool marked);
    void refreshRememberedOutline();
    void setCurrentRowInternal(int row, bool clearRememberedOutline);
    void applyMarkedAlbumSelection();
    void updateRubberBandSelection();
    void finishRubberBandSelection();
    void updateDragAutoscroll();
    void stopDragSelection();
    // Drag-select rubber band: the start corner is anchored in content space so
    // it tracks the items as autoscroll moves the view; setRubberBandGeometry
    // also repaints the swept region so the moving border leaves no trail.
    QRect currentDragRect() const;
    void setRubberBandGeometry(const QRect &rect);
    QString titleForRow(int row) const;

private:
    ArtworkCache *m_artworkCache = nullptr;
    QString m_selectedAlbumTitle;
    QSet<QString> m_markedAlbumTitles;
    bool m_rememberedOutlineVisible = false;
    QVector<Album> m_sourceAlbums;  // unsorted source list from last setAlbums call
    QVector<Album> m_pendingAlbums;
    QTimer *m_populateTimer = nullptr;
    QTimer *m_artworkTimer = nullptr;
    QTimer *m_spinnerTimer = nullptr;
    QTimer *m_dragScrollTimer = nullptr;
    QRubberBand *m_rubberBand = nullptr;
    QSet<QString> m_dragBaseMarkedAlbumTitles;
    QPoint m_dragStartPos;
    QPoint m_dragCurrentPos;
    int m_dragStartScroll = 0;  // vertical scroll value when the drag began
    Qt::KeyboardModifiers m_dragModifiers = Qt::NoModifier;
    int m_shiftAnchorRow = -1;
    bool m_leftButtonPressed = false;
    bool m_dragSelecting = false;
    bool m_pressStartedOnRating = false;
    bool m_showLoading = false;
    int m_loadingCount = 0;
    int m_loadingAngle = 0;
    qsizetype m_nextAlbumRow = 0;
    int m_nextArtworkRow = 0;
    int m_artworkGeneration = 0;
    int m_cellWidth = 204;   // configured base (C-scroll target / persisted)
    int m_cellHeight = 292;
    int m_artSize = 176;
    int m_effectiveCellWidth = 204;  // base stretched to fill the viewport width
    int m_effectiveCellHeight = 292;
    int m_effectiveArtSize = 176;
    int m_spacing = 6;
    int m_padding = 8;
    int m_starSize = 18;
    Qt::Alignment m_textAlignment = Qt::AlignHCenter;
    MusicSort::SortField m_sortField = MusicSort::SortField::Year;
    bool m_sortDescending = true; // newest first for Year
    bool m_sortReverseGroups = false;
    QPoint m_wheelAngleRemainder;
};
