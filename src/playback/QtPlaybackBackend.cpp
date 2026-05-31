#include "playback/QtPlaybackBackend.h"

#include <QAudioOutput>
#include <QMediaPlayer>

#include <algorithm>

namespace {

PlaybackBackend::State toBackendState(QMediaPlayer::PlaybackState state)
{
    switch (state) {
    case QMediaPlayer::StoppedState:
        return PlaybackBackend::State::Stopped;
    case QMediaPlayer::PlayingState:
        return PlaybackBackend::State::Playing;
    case QMediaPlayer::PausedState:
        return PlaybackBackend::State::Paused;
    }
    return PlaybackBackend::State::Stopped;
}

} // namespace

QtPlaybackBackend::QtPlaybackBackend(QObject *parent)
    : PlaybackBackend(parent)
{
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(1.0);

    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOutput);

    connect(m_player, &QMediaPlayer::positionChanged, this, &PlaybackBackend::positionChanged);
    connect(m_player, &QMediaPlayer::durationChanged, this, &PlaybackBackend::durationChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        m_state = toBackendState(state);
        emit stateChanged(m_state);
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            emit finished();
        }
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &errorString) {
        m_state = State::Error;
        emit stateChanged(m_state);
        emit errorOccurred(errorString);
    });
}

void QtPlaybackBackend::setProfile(const PlaybackProfile &profile)
{
    Q_UNUSED(profile)
    emit technicalInfoChanged(QStringLiteral("Qt Multimedia backend"));
}

void QtPlaybackBackend::play(const QUrl &url)
{
    if (url.isEmpty()) {
        return;
    }
    m_player->setSource(url);
    m_player->play();
}

void QtPlaybackBackend::loadPaused(const QUrl &url)
{
    if (url.isEmpty()) {
        return;
    }
    m_player->setSource(url);
    m_player->pause();
}

void QtPlaybackBackend::prepareNext(const QUrl &url)
{
    Q_UNUSED(url)
}

void QtPlaybackBackend::pause()
{
    m_player->pause();
}

void QtPlaybackBackend::resume()
{
    m_player->play();
}

void QtPlaybackBackend::stop()
{
    m_player->stop();
}

void QtPlaybackBackend::seek(qint64 positionMs)
{
    m_player->setPosition(positionMs);
}

void QtPlaybackBackend::setVolume(double volume0To1)
{
    m_audioOutput->setVolume(static_cast<float>(std::clamp(volume0To1, 0.0, 1.0)));
}

PlaybackBackend::State QtPlaybackBackend::state() const
{
    return m_state;
}

bool QtPlaybackBackend::hasSource() const
{
    return !m_player->source().isEmpty();
}

qint64 QtPlaybackBackend::position() const
{
    return m_player->position();
}

qint64 QtPlaybackBackend::duration() const
{
    return m_player->duration();
}
