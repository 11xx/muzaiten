#pragma once

#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "core/Track.h"
#include "search/SearchMatcher.h"
#include "ui/QueueTable.h"

class QImage;
class QLabel;
class QueueStore;
class QSplitter;
class QTableWidget;
class QWidget;

class RightSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit RightSidebar(QWidget *parent = nullptr);

    void setQueueStore(QueueStore *store);
    void setQueue(const QVector<Track> &tracks);
    void setPlayNextRange(int begin, int end);
    void setCurrentIndex(int index, bool reveal = false);
    void setAlbumArt(const QString &imagePath);
    void setAlbumArt(const QImage &image);
    void setTrackInfo(const Track &track);
    void setTrackInfoVisible(bool visible);
    void configureTrackInfoPanel(QWidget *parent);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    void setHeaderHeight(int height);
    void setNavigationScrollPadding(int rows);
    QWidget *queueNavigationWidget() const;
    int queueRowCount() const;
    int queueCurrentRow() const;
    void setQueueCurrentRow(int row);
    void setQueueCurrentRow(int row, int scrollDirection);
    void moveQueueCurrentRow(int delta);
    void activateCurrentQueueTrack();
    QVector<Search::MatchDocument> queueSearchDocuments() const;

signals:
    void queueTrackActivated(int index);
    void queueTrackRatingChanged(const Track &track, int rating0To100);
    void queueRowsMoveRequested(const QVector<int> &rows, int destinationRow);
    void queueRowsRemoveRequested(const QVector<int> &rows);
    void removeAllMissingTracksRequested();
    void queueClearRequested();
    void clearPlayNextPriorityRequested();
    void saveQueueAsRequested();
    void restorePreviousQueueRequested();
    void artistRequested(const QString &artistName);
    void albumRequested(const QString &artistName, const QString &albumTitle);
    void findFileRequested(const Track &track);
    void propertiesRequested(const Track &track);
    void trackLibraryRequested(const Track &track);
    void viewSettingsChanged();

private:
    void showTrackInfoLabelMenu(const QPoint &pos);
    void applyTrackInfoSettingsJson(const QJsonObject &root);
    QJsonArray trackInfoSettingsJson() const;
    QJsonArray trackInfoMetadataSettingsJson() const;
    void updateTrackInfoLabels();
    void restyleTrackInfoLabels();
    void applyTrackInfoLayoutSpacing();
    QWidget *trackInfoLabelFromSender() const;

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QueueTable *m_queueTable = nullptr;
    QueueStore *m_queueStore = nullptr;
    QLabel *m_albumArt = nullptr;
    QWidget *m_trackInfoPane = nullptr;
    QWidget *m_trackInfoTitle = nullptr;
    QWidget *m_trackInfoArtist = nullptr;
    QWidget *m_trackInfoAlbum = nullptr;
    QWidget *m_trackInfoYear = nullptr;
    QWidget *m_trackInfoFile = nullptr;
    QWidget *m_trackInfoProperties = nullptr;
    QWidget *m_trackInfoTopSpacer = nullptr;
    QWidget *m_trackInfoBottomSpacer = nullptr;
    QLabel *m_noTrackLabel = nullptr;
    QSplitter *m_splitter = nullptr;
    QVector<Track> m_tracks;
    Track m_currentTrack;
    QJsonArray m_trackInfoMetadataItems;
    QString m_trackInfoMetadataSeparator = QString::fromUtf8("\xc2\xb7");
    int m_trackInfoMetadataSpacing = 1;
    QString m_trackInfoAlignment = QStringLiteral("left");
    QString m_trackInfoLineSpacingMode = QStringLiteral("justify");
    int m_trackInfoLineSpacing = 1;
    QString m_trackInfoVerticalAlignment = QStringLiteral("top");
    int m_queueHoveredRow = -1;
    int m_queueDropIndicatorRow = -1;
    int m_currentQueueIndex = -1;
    int m_playNextBegin = -1;
    int m_playNextEnd = -1;
    bool m_showPlayNextBadge = true;
    bool m_showPlayNextTitleAccent = false;
    bool m_usingArtFallback = true;
};
