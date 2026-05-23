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

signals:
    void queueTrackActivated(int index);

private:
    QTableWidget *m_queueTable = nullptr;
    QLabel *m_albumArt = nullptr;
};
