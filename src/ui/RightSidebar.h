#pragma once

#include <QWidget>

#include "core/Track.h"

class QLabel;
class QTableWidget;

class RightSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit RightSidebar(QWidget *parent = nullptr);

    void setQueue(const QVector<Track> &tracks);
    void setCurrentIndex(int index);
    void setAlbumArt(const QString &imagePath);
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
    QVector<Track> m_tracks;
};
