#pragma once

#include "playback/PlaybackTypes.h"

#include <QObject>
#include <QUrl>

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

signals:
    void stateChanged(PlaybackBackend::State state);
    void positionChanged(qint64 positionMs);
    void durationChanged(qint64 durationMs);
    void aboutToNeedNext();
    void preparedTrackStarted();
    void finished();
    void errorOccurred(QString message);
    void technicalInfoChanged(QString text);
};
