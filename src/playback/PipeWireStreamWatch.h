#pragma once

#include <QObject>
#include <QString>

#include <memory>

// Follows where one native PipeWire playback stream is linked: the sink its
// node feeds, as the graph's own link events report it, so a move made in a
// mixer is seen the moment it happens. It knows nothing else about the graph
// beyond sink names. Optional: without PipeWire support in the build, or
// without a reachable daemon, start() fails and nothing is reported.
class PipeWireStreamWatch final : public QObject {
    Q_OBJECT

public:
    explicit PipeWireStreamWatch(QObject *parent = nullptr);
    ~PipeWireStreamWatch() override;

    // Whether the build can follow native streams at all.
    static bool supported();

    // Follow the stream whose node is named |nodeName|, across reconnects
    // under the same name. Returns false when PipeWire cannot be reached.
    bool start(const QString &nodeName);
    void stop();
    bool running() const;

    // The sink the stream is linked to now, or empty (not linked, or moving).
    QString linkedSink() const;
    // Move the live stream to |sinkName| the way a mixer does, through the
    // default metadata, without reopening it. False when the stream or the
    // sink is not known.
    bool moveTo(const QString &sinkName);

signals:
    void linkedSinkChanged(const QString &sink);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
