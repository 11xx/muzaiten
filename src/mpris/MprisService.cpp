#include "mpris/MprisService.h"

#include "Version.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QFileInfo>
#include <QIODevice>
#include <QUrl>

#include <algorithm>

namespace {
constexpr auto objectPath = "/org/mpris/MediaPlayer2";
constexpr auto rootInterface = "org.mpris.MediaPlayer2";
constexpr auto playerInterface = "org.mpris.MediaPlayer2.Player";

qlonglong msToUsec(qint64 value)
{
    return static_cast<qlonglong>(std::max<qint64>(0, value) * 1000);
}

QString titleForTrack(const Track &track)
{
    if (!track.title.trimmed().isEmpty()) {
        return track.title.trimmed();
    }
    return track.filename.trimmed().isEmpty() ? QFileInfo(track.path).fileName() : track.filename.trimmed();
}

QString artistForTrack(const Track &track)
{
    return track.artistName.trimmed().isEmpty() ? track.albumArtistName.trimmed() : track.artistName.trimmed();
}

class MprisRootAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ CanQuit)
    Q_PROPERTY(bool CanRaise READ CanRaise)
    Q_PROPERTY(bool HasTrackList READ HasTrackList)
    Q_PROPERTY(QString Identity READ Identity)
    Q_PROPERTY(QString DesktopEntry READ DesktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ SupportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ SupportedMimeTypes)

public:
    explicit MprisRootAdaptor(MprisService *service)
        : QDBusAbstractAdaptor(service)
        , m_service(service)
    {
        setAutoRelaySignals(true);
    }

    bool CanQuit() const { return false; }
    bool CanRaise() const { return true; }
    bool HasTrackList() const { return false; }
    QString Identity() const { return QStringLiteral(MUZAITEN_APP_NAME); }
    QString DesktopEntry() const { return QStringLiteral(MUZAITEN_APP_ID); }
    QStringList SupportedUriSchemes() const { return {QStringLiteral("file")}; }
    QStringList SupportedMimeTypes() const { return {}; }

public slots:
    void Raise() { emit m_service->raiseRequested(); }
    void Quit() {}

private:
    MprisService *m_service = nullptr;
};

class MprisPlayerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ PlaybackStatus)
    Q_PROPERTY(QString LoopStatus READ LoopStatus WRITE SetLoopStatus)
    Q_PROPERTY(double Rate READ Rate WRITE SetRate)
    Q_PROPERTY(bool Shuffle READ Shuffle WRITE SetShuffle)
    Q_PROPERTY(QVariantMap Metadata READ Metadata)
    Q_PROPERTY(double Volume READ Volume WRITE SetVolume)
    Q_PROPERTY(qlonglong Position READ Position)
    Q_PROPERTY(double MinimumRate READ MinimumRate)
    Q_PROPERTY(double MaximumRate READ MaximumRate)
    Q_PROPERTY(bool CanGoNext READ CanGoNext)
    Q_PROPERTY(bool CanGoPrevious READ CanGoPrevious)
    Q_PROPERTY(bool CanPlay READ CanPlay)
    Q_PROPERTY(bool CanPause READ CanPause)
    Q_PROPERTY(bool CanSeek READ CanSeek)
    Q_PROPERTY(bool CanControl READ CanControl)

public:
    explicit MprisPlayerAdaptor(MprisService *service)
        : QDBusAbstractAdaptor(service)
        , m_service(service)
    {
        setAutoRelaySignals(true);
    }

    QString PlaybackStatus() const { return m_service->playbackStatus(); }
    QString LoopStatus() const { return QStringLiteral("None"); }
    void SetLoopStatus(const QString &) {}
    double Rate() const { return 1.0; }
    void SetRate(double) {}
    bool Shuffle() const { return false; }
    void SetShuffle(bool) {}
    QVariantMap Metadata() const { return m_service->metadata(); }
    double Volume() const { return m_service->volume(); }
    void SetVolume(double volume) { emit m_service->volumeRequested(std::clamp(volume, 0.0, 1.0)); }
    qlonglong Position() const { return m_service->positionUsec(); }
    double MinimumRate() const { return 1.0; }
    double MaximumRate() const { return 1.0; }
    bool CanGoNext() const { return m_service->canGoNext(); }
    bool CanGoPrevious() const { return m_service->canGoPrevious(); }
    bool CanPlay() const { return m_service->canPlay(); }
    bool CanPause() const { return m_service->canPause(); }
    bool CanSeek() const { return m_service->canSeek(); }
    bool CanControl() const { return true; }

public slots:
    void Next() { emit m_service->nextRequested(); }
    void Previous() { emit m_service->previousRequested(); }
    void Pause() { emit m_service->pauseRequested(); }
    void PlayPause() { emit m_service->playPauseRequested(); }
    void Stop() { emit m_service->stopRequested(); }
    void Play() { emit m_service->playRequested(); }
    void Seek(qlonglong offsetUsec)
    {
        emit m_service->relativeSeekRequested(offsetUsec / 1000);
        emit Seeked(std::max<qlonglong>(0, m_service->positionUsec() + offsetUsec));
    }
    void SetPosition(const QDBusObjectPath &, qlonglong positionUsec)
    {
        emit m_service->seekRequested(positionUsec / 1000);
        emit Seeked(positionUsec);
    }
    void OpenUri(const QString &) {}

signals:
    void Seeked(qlonglong positionUsec);

private:
    MprisService *m_service = nullptr;
};

} // namespace

MprisService::MprisService(QObject *parent)
    : QObject(parent)
{
    new MprisRootAdaptor(this);
    new MprisPlayerAdaptor(this);

    auto bus = QDBusConnection::sessionBus();
    const QString baseName = QStringLiteral("org.mpris.MediaPlayer2.muzaiten");
    m_serviceName = baseName;
    bool serviceRegistered = bus.registerService(m_serviceName);
    if (!serviceRegistered) {
        m_serviceName = QStringLiteral("%1.instance%2").arg(baseName).arg(QCoreApplication::applicationPid());
        serviceRegistered = bus.registerService(m_serviceName);
    }
    const bool objectRegistered = bus.registerObject(QString::fromLatin1(objectPath), this, QDBusConnection::ExportAdaptors);
    m_registered = bus.isConnected() && serviceRegistered && objectRegistered;
}

bool MprisService::isRegistered() const
{
    return m_registered;
}

QString MprisService::serviceName() const
{
    return m_serviceName;
}

QString MprisService::playbackStatus() const
{
    switch (m_state) {
    case PlaybackBackend::State::Playing:
    case PlaybackBackend::State::Buffering:
        return QStringLiteral("Playing");
    case PlaybackBackend::State::Paused:
        return QStringLiteral("Paused");
    case PlaybackBackend::State::Stopped:
    case PlaybackBackend::State::Error:
        return QStringLiteral("Stopped");
    }
    return QStringLiteral("Stopped");
}

QVariantMap MprisService::metadata() const
{
    return buildMetadata(m_track);
}

double MprisService::volume() const
{
    return m_volume;
}

qlonglong MprisService::positionUsec() const
{
    return msToUsec(m_positionMs);
}

qlonglong MprisService::durationUsec() const
{
    return msToUsec(m_durationMs);
}

bool MprisService::canGoNext() const
{
    return m_canGoNext;
}

bool MprisService::canGoPrevious() const
{
    return m_canGoPrevious;
}

bool MprisService::canPlay() const
{
    return m_canPlay;
}

bool MprisService::canPause() const
{
    return m_state == PlaybackBackend::State::Playing || m_state == PlaybackBackend::State::Buffering;
}

bool MprisService::canSeek() const
{
    return m_durationMs > 0;
}

void MprisService::setTrack(const Track &track)
{
    m_track = track;
    if (track.durationMs > 0) {
        m_durationMs = track.durationMs;
    }
    emitPropertiesChanged(QString::fromLatin1(playerInterface),
                          {{QStringLiteral("Metadata"), metadata()},
                           {QStringLiteral("CanSeek"), canSeek()},
                           {QStringLiteral("CanPlay"), canPlay()}});
}

void MprisService::setPlaybackState(PlaybackBackend::State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emitPropertiesChanged(QString::fromLatin1(playerInterface),
                          {{QStringLiteral("PlaybackStatus"), playbackStatus()},
                           {QStringLiteral("CanPause"), canPause()}});
}

void MprisService::setPositionMs(qint64 positionMs)
{
    m_positionMs = std::max<qint64>(0, positionMs);
}

void MprisService::setDurationMs(qint64 durationMs)
{
    const qint64 duration = std::max<qint64>(0, durationMs);
    if (m_durationMs == duration) {
        return;
    }
    m_durationMs = duration;
    emitPropertiesChanged(QString::fromLatin1(playerInterface),
                          {{QStringLiteral("Metadata"), metadata()},
                           {QStringLiteral("CanSeek"), canSeek()}});
}

void MprisService::setVolume(double volume0To1)
{
    const double volume = std::clamp(volume0To1, 0.0, 1.0);
    if (qFuzzyCompare(m_volume, volume)) {
        return;
    }
    m_volume = volume;
    emitPropertiesChanged(QString::fromLatin1(playerInterface), {{QStringLiteral("Volume"), m_volume}});
}

void MprisService::setQueueCapabilities(bool canGoPrevious, bool canGoNext, bool canPlay)
{
    m_canGoPrevious = canGoPrevious;
    m_canGoNext = canGoNext;
    m_canPlay = canPlay;
    emitPropertiesChanged(QString::fromLatin1(playerInterface),
                          {{QStringLiteral("CanGoPrevious"), m_canGoPrevious},
                           {QStringLiteral("CanGoNext"), m_canGoNext},
                           {QStringLiteral("CanPlay"), m_canPlay}});
}

void MprisService::emitPropertiesChanged(const QString &interfaceName, const QVariantMap &changedProperties)
{
    if (!m_registered || changedProperties.isEmpty()) {
        return;
    }

    QDBusMessage message = QDBusMessage::createSignal(QString::fromLatin1(objectPath),
                                                      QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      QStringLiteral("PropertiesChanged"));
    message << interfaceName << changedProperties << QStringList{};
    QDBusConnection::sessionBus().send(message);
}

QVariantMap MprisService::buildMetadata(const Track &track) const
{
    QVariantMap map;
    map.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(trackObjectPath(track))));
    if (track.durationMs > 0 || m_durationMs > 0) {
        map.insert(QStringLiteral("mpris:length"), msToUsec(track.durationMs > 0 ? track.durationMs : m_durationMs));
    }
    if (!track.path.isEmpty()) {
        map.insert(QStringLiteral("xesam:url"), QUrl::fromLocalFile(track.path).toString());
    }
    const QString title = titleForTrack(track);
    if (!title.isEmpty()) {
        map.insert(QStringLiteral("xesam:title"), title);
    }
    const QString artist = artistForTrack(track);
    if (!artist.isEmpty()) {
        map.insert(QStringLiteral("xesam:artist"), QStringList{artist});
    }
    if (!track.albumArtistName.trimmed().isEmpty()) {
        map.insert(QStringLiteral("xesam:albumArtist"), QStringList{track.albumArtistName.trimmed()});
    }
    if (!track.albumTitle.trimmed().isEmpty()) {
        map.insert(QStringLiteral("xesam:album"), track.albumTitle.trimmed());
    }
    if (track.trackNumber > 0) {
        map.insert(QStringLiteral("xesam:trackNumber"), track.trackNumber);
    }
    if (!track.date.trimmed().isEmpty()) {
        map.insert(QStringLiteral("xesam:contentCreated"), track.date.trimmed());
    }
    if (track.effectiveRating0To100 >= 0) {
        map.insert(QStringLiteral("xesam:userRating"), static_cast<double>(track.effectiveRating0To100) / 100.0);
    }
    return map;
}

QString MprisService::trackObjectPath(const Track &track) const
{
    if (track.path.isEmpty()) {
        return QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack");
    }
    const QByteArray hash = QCryptographicHash::hash(track.path.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("/org/mpris/MediaPlayer2/TrackList/%1").arg(QString::fromLatin1(hash));
}

#include "MprisService.moc"
