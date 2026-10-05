#pragma once

#include "playback/AudioDeviceControl.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

// Resolves the direct card right before it is used. Card numbers change only
// when a sound card appears or disappears, which shows as an entry added to or
// removed from /dev/snd; a resolved card is reused until then, so a check
// before every open costs no PipeWire query while nothing was plugged.
class DirectDeviceResolver final : public QObject {
    Q_OBJECT

public:
    explicit DirectDeviceResolver(QObject *parent = nullptr);

    AudioDeviceControl::DirectTarget resolve(const QString &stableId, const QString &savedDevice);
    // Forget the reused answer.
    void invalidate();

signals:
    // A sound card appeared or went away, after PipeWire had a moment to
    // register the change.
    void cardsChanged();

private:
    void handleDeviceNodesChanged();

    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    bool m_watching = false;
    QString m_cachedId;
    AudioDeviceControl::DirectTarget m_cached;
};
