#include "playback/DirectDeviceResolver.h"

DirectDeviceResolver::DirectDeviceResolver(QObject *parent)
    : QObject(parent)
{
    m_watching = m_watcher.addPath(QStringLiteral("/dev/snd"));
    m_settle.setSingleShot(true);
    m_settle.setInterval(1500);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &DirectDeviceResolver::handleDeviceNodesChanged);
    connect(&m_settle, &QTimer::timeout, this, [this]() {
        invalidate();
        emit cardsChanged();
    });
}

AudioDeviceControl::DirectTarget DirectDeviceResolver::resolve(const QString &stableId,
                                                               const QString &savedDevice)
{
    using Status = AudioDeviceControl::DirectTarget::Status;
    if (m_watching && !stableId.isEmpty() && stableId == m_cachedId) {
        return m_cached;
    }
    const AudioDeviceControl::DirectTarget target = AudioDeviceControl::resolveDirect(stableId, savedDevice);
    // Only a found card is reused; an absent one is looked for again at the
    // next use, since PipeWire may register it after the device node appears.
    if (target.status == Status::Resolved) {
        m_cachedId = stableId;
        m_cached = target;
    } else {
        invalidate();
    }
    return target;
}

void DirectDeviceResolver::handleDeviceNodesChanged()
{
    invalidate();
    m_settle.start();
}

void DirectDeviceResolver::invalidate()
{
    m_cachedId.clear();
    m_cached = {};
}
