#pragma once

#include <QWidget>

#include "core/Track.h"

class QLabel;
class QSplitter;
class QTableWidget;
class QWidget;

class RightSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit RightSidebar(QWidget *parent = nullptr);

    void setQueue(const QVector<Track> &tracks);
    void setCurrentIndex(int index);
    void setAlbumArt(const QString &imagePath);
    void setTrackInfo(const Track &track);
    void setTrackInfoVisible(bool visible);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void setHeaderHeight(int height);

signals:
    void queueTrackActivated(int index);
    void findFileRequested(const Track &track);
    void viewSettingsChanged();

private:
    void showHeaderMenu(const QPoint &pos);
    void showQueueMenu(const QPoint &pos);

private:
    QTableWidget *m_queueTable = nullptr;
    QLabel *m_albumArt = nullptr;
    QWidget *m_trackInfoPane = nullptr;
    QLabel *m_trackInfoTitle = nullptr;
    QLabel *m_trackInfoArtist = nullptr;
    QLabel *m_trackInfoAlbum = nullptr;
    QLabel *m_trackInfoYear = nullptr;
    QLabel *m_trackInfoFile = nullptr;
    QLabel *m_trackInfoProperties = nullptr;
    QSplitter *m_splitter = nullptr;
    QVector<Track> m_tracks;
    int m_rowHeight = 20;
};
