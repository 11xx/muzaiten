#pragma once

#include "playback/PlaybackTypes.h"

#include <QObject>
#include <QUrl>
#include <QVector>

#include <functional>

// A shared playback endpoint as the sound server names it: `name` is the stable
// identity (PipeWire node.name / PulseAudio sink name), `label` its description.
struct OutputEndpoint {
    QString name;
    QString label;
};

// Where output is going, for display. `currentEndpoint` is where the stream
// is linked right now, when the transport can tell; it may differ from the
// preference while a fallback is active.
struct OutputStatus {
    enum class Route {
        System,      // the session manager chooses
        Preferred,   // the preferred endpoint is present and requested
        Waiting,     // the preferred output is missing; playback waits for it
        Fallback,    // the preferred endpoint is missing; the system route plays
        Direct,      // exclusive ALSA output
        Unsupported  // a destination is configured that this transport cannot honor
    };
    Route route = Route::System;
    QString transport;      // what renders shared audio, e.g. "PulseAudio"
    QString transportNote;  // why that transport, when it is not the one asked for
    QString preferredEndpoint;
    QString preferredLabel;
    bool preferredAvailable = false;
    QString currentEndpoint;
    QString currentLabel;

    bool operator==(const OutputStatus &) const = default;
};

class PlaybackBackend : public QObject {
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Playing,
        Paused,
        Buffering,
        Error
    };
    Q_ENUM(State)

    // How the next play()/loadPaused() renders audio. NativeDsd builds a
    // dedicated DSD-passthrough pipeline straight to the DAC (bit-perfect);
    // Normal uses the ordinary decode path (incl. live DSD→PCM). Sticky until
    // changed via setOutputMode().
    enum class OutputMode { Normal, NativeDsd };
    Q_ENUM(OutputMode)

    // What DSD playback the backend can actually do given the installed plugins.
    // Independent flags: a system may decode DSD→PCM, pass DSD through natively,
    // both, or neither. Lets the orchestration warn precisely about what's
    // missing instead of silently failing on a DSD track.
    struct DsdSupport {
        bool nativePassthrough = false;
        bool pcmDecode = false;
    };

    explicit PlaybackBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~PlaybackBackend() override = default;

    virtual void setProfile(const PlaybackProfile &profile) = 0;
    virtual void play(const QUrl &url) = 0;
    // Load the source into a prerolled-but-paused state without ever producing
    // audio output. Default falls back to play()+pause() for backends that
    // cannot distinguish the two; backends that can should override.
    virtual void loadPaused(const QUrl &url) { play(url); pause(); }
    virtual void prepareNext(const QUrl &url) = 0;
    virtual void setGaplessStopPending(bool pending)
    {
        Q_UNUSED(pending);
    }
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void seek(qint64 positionMs) = 0;
    virtual void setVolume(double volume0To1) = 0;
    virtual State state() const = 0;
    virtual bool hasSource() const = 0;
    virtual qint64 position() const = 0;
    virtual qint64 duration() const = 0;
    // Resolve the boundary before a queue/policy mutation changes the identity
    // of the prepared row. Backends without gapless preloading have nothing to
    // stabilize.
    virtual void stabilizeGaplessHandoff() {}
    // Called after a gapless track advance so the backend can re-point any
    // per-track resources (e.g. read-ahead) at the newly-current track.
    virtual void onGaplessTrackAdvanced() {}

    // Select how the next play()/loadPaused() renders. For NativeDsd, dsdDevice
    // is the ALSA "hw:N" to open exclusively (empty → use the profile device).
    // No-op default keeps backends that don't support DSD passthrough working.
    virtual void setOutputMode(OutputMode mode, const QString &dsdDevice = QString())
    {
        Q_UNUSED(mode);
        Q_UNUSED(dsdDevice);
    }
    // DSD capability of this backend with the currently-installed plugins.
    virtual DsdSupport dsdSupport() const { return {}; }

    // Whether this backend can route shared output to a chosen endpoint.
    // Backends that cannot report the configured destination as Unsupported
    // instead of silently ignoring it.
    virtual bool supportsSharedEndpoint() const { return false; }
    virtual OutputStatus outputStatus() const { return {}; }
    // Counts requests that set what plays and from where: starts, seeks, stops
    // and output changes. A step delayed on behalf of one of them compares it
    // to learn whether something newer was asked for since. Pausing and
    // resuming leave what plays alone and are not counted.
    quint64 transportRequests() const { return m_transportRequests; }

    // The direct (exclusive ALSA) device as resolved at the moment of opening
    // it, from the profile's stable id and last known device string.
    // Unavailable means the selected card is not present: the backend then
    // waits rather than opening whatever holds its old number.
    struct DirectDevice {
        bool available = false;
        QString hwPath;
        QString label;
    };
    using DirectDeviceCheck = std::function<DirectDevice(const QString &stableId, const QString &savedDevice)>;
    // Consulted before every direct open, retry and native DSD start.
    virtual void setDirectDeviceCheck(DirectDeviceCheck check) { Q_UNUSED(check); }
    // The cards may have changed: an open waiting for the direct device
    // checks again.
    virtual void retryDirectDevice() {}

    // The endpoints a shared transport choice ("auto", "pipewire", ...) can
    // reach right now, for choosing one.
    virtual QVector<OutputEndpoint> sharedEndpoints(const QString &sinkChoice) const
    {
        Q_UNUSED(sinkChoice);
        return {};
    }

protected:
    void noteTransportRequest() { ++m_transportRequests; }

signals:
    void stateChanged(PlaybackBackend::State state);
    void positionChanged(qint64 positionMs);
    void durationChanged(qint64 durationMs);
    void aboutToNeedNext();
    void preparedTrackStarted();
    void finished();
    void errorOccurred(QString message);
    void technicalInfoChanged(QString text);
    void outputStatusChanged();
    // A mixer moved playback off the preferred endpoint while that endpoint
    // stayed available: a deliberate choice made outside the app, which the
    // backend now requests in its place.
    void sharedEndpointChosenExternally(QString name, QString label);

private:
    quint64 m_transportRequests = 0;
};
