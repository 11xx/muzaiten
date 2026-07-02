#pragma once

#include <QWidget>

#include "core/Track.h"

#include <functional>

class QueueStore;
class QueueTable;

class QueueScreen final : public QWidget {
    Q_OBJECT

public:
    explicit QueueScreen(QWidget *parent = nullptr);

    void setQueueStore(QueueStore *store);
    void setPickReasonResolver(std::function<QString(const QString &)> resolver);
    void setTrackFlagResolver(std::function<bool(const Track &, const QString &)> resolver);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    void setNavigationScrollPadding(int rows);
    void setKeyBindingProfileName(const QString &name);
    QString keyBindingProfileName() const;
    void focusQueue();
    void revealCurrentPlaying();
    void setQueueIsPlaylistSourced(bool sourced);

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
    void unlinkQueueFromPlaylistRequested();
    void findFileRequested(const Track &track);
    void addToPlaylistRequested(const QVector<Track> &tracks);
    void propertiesRequested(const Track &track);
    void startRadioRequested(const Track &track);
    void trackFlagChanged(const Track &track, const QString &flag, bool on);
    void trackLibraryRequested(const Track &track);
    void viewSettingsChanged();

private:
    QueueTable *m_table = nullptr;
};
