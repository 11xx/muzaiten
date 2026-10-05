#pragma once

#include "playback/PlaybackBackend.h"

#include <QHash>
#include <QObject>

using GstDeviceProvider = struct _GstDeviceProvider;

// The playback endpoints of a shared transport, kept current from the sound
// server's own device events. It watches sinks only, through the GStreamer
// device provider matching the transport, so the names it reports are the ones
// that transport's sink accepts as a target, virtual sinks included.
class OutputEndpointMonitor final : public QObject {
    Q_OBJECT

public:
    explicit OutputEndpointMonitor(QObject *parent = nullptr);
    ~OutputEndpointMonitor() override;

    // The device provider serving a sink factory ("pulsesink" or
    // "pipewiresink"), or empty for sinks without addressable endpoints.
    static QString providerFor(const QString &sinkFactory);
    // One-off listing without keeping a connection to the server.
    static QVector<OutputEndpoint> probe(const QString &sinkFactory);

    // Start watching for |sinkFactory|; the endpoint list is complete when
    // this returns. Returns false when no provider serves that factory.
    bool start(const QString &sinkFactory);
    void stop();
    bool running() const { return m_provider != nullptr; }

    bool contains(const QString &name) const { return m_endpoints.contains(name); }
    QString label(const QString &name) const { return m_endpoints.value(name); }
    QVector<OutputEndpoint> endpoints() const;

signals:
    void endpointAdded(const QString &name);
    void endpointRemoved(const QString &name);

private:
    void add(const QString &name, const QString &label);
    void remove(const QString &name);

    GstDeviceProvider *m_provider = nullptr;
    QHash<QString, QString> m_endpoints;
    // The current run, bumped by every start() and stop(). A device event
    // queued by a provider run that has since ended is dropped on delivery,
    // so it cannot change a newer run's list.
    quint64 m_run = 0;
};
