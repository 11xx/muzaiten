#pragma once

#include <QWidget>

#include "core/Track.h"

class QueueStore;
class QueueTable;

class QueueScreen final : public QWidget {
    Q_OBJECT

public:
    explicit QueueScreen(QWidget *parent = nullptr);

    void setQueueStore(QueueStore *store);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    void setNavigationScrollPadding(int rows);
    void setKeyBindingProfileName(const QString &name);
    QString keyBindingProfileName() const;
    void focusQueue();
    void revealCurrentPlaying();

signals:
    void queueTrackActivated(int index);
    void queueTrackRatingChanged(const Track &track, int rating0To100);
    void queueRowsMoveRequested(const QVector<int> &rows, int destinationRow);
    void queueRowsRemoveRequested(const QVector<int> &rows);
    void queueClearRequested();
    void clearPlayNextPriorityRequested();
    void saveQueueAsRequested();
    void restorePreviousQueueRequested();
    void mergeSavedQueueRequested();
    void findFileRequested(const Track &track);
    void propertiesRequested(const Track &track);
    void trackLibraryRequested(const Track &track);
    void viewSettingsChanged();

private:
    QueueTable *m_table = nullptr;
};
