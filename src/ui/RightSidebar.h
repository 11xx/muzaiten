#pragma once

#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>

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
    void configureTrackInfoPanel(QWidget *parent);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void setHeaderHeight(int height);

signals:
    void queueTrackActivated(int index);
    void artistRequested(const QString &artistName);
    void albumRequested(const QString &artistName, const QString &albumTitle);
    void findFileRequested(const Track &track);
    void viewSettingsChanged();

private:
    void showHeaderMenu(const QPoint &pos);
    void showQueueMenu(const QPoint &pos);
    void applyTrackInfoSettingsJson(const QJsonObject &root);
    QJsonArray trackInfoSettingsJson() const;
    void updateTrackInfoLabels();
    void restyleTrackInfoLabels();

protected:
    void changeEvent(QEvent *event) override;

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
    QLabel *m_noTrackLabel = nullptr;
    QSplitter *m_splitter = nullptr;
    QVector<Track> m_tracks;
    Track m_currentTrack;
    QString m_trackInfoMetadataPattern;
};
