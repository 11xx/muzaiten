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
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {
constexpr auto objectPath = "/org/mpris/MediaPlayer2";
constexpr auto rootInterface = "org.mpris.MediaPlayer2";
constexpr auto playerInterface = "org.mpris.MediaPlayer2.Player";
constexpr auto muzaitenPlayerInterface = "org.muzaiten.Player";

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

double secondsFromMs(qint64 value)
{
    return static_cast<double>(std::max<qint64>(0, value)) / 1000.0;
}

// Trim emitted doubles to 2 decimals — nothing in the status JSON (seconds,
// percent, star ratings) is meaningful beyond centisecond / 0.01 precision.
double round2(double value)
{
    return std::round(value * 100.0) / 100.0;
}

QString ratingSourceName(Rating::Source source)
{
    switch (source) {
    case Rating::Source::None:
        return QStringLiteral("none");
    case Rating::Source::MusicBeeCompatible:
        return QStringLiteral("musicbee_compatible");
    case Rating::Source::VorbisRating:
        return QStringLiteral("vorbis_rating");
    case Rating::Source::Id3Popularimeter:
        return QStringLiteral("id3_popularimeter");
    case Rating::Source::Mp4Rate:
        return QStringLiteral("mp4_rate");
    case Rating::Source::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString stateName(PlaybackBackend::State state)
{
    switch (state) {
    case PlaybackBackend::State::Playing:
        return QStringLiteral("playing");
    case PlaybackBackend::State::Buffering:
        return QStringLiteral("buffering");
    case PlaybackBackend::State::Paused:
        return QStringLiteral("paused");
    case PlaybackBackend::State::Stopped:
        return QStringLiteral("stopped");
    case PlaybackBackend::State::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("stopped");
}

QString repeatModeName(RepeatMode mode)
{
    switch (mode) {
    case RepeatMode::One:
        return QStringLiteral("one");
    case RepeatMode::All:
        return QStringLiteral("all");
    case RepeatMode::Off:
        break;
    }
    return QStringLiteral("off");
}

QString shuffleModeName(ShuffleMode mode)
{
    switch (mode) {
    case ShuffleMode::Queue:
        return QStringLiteral("queue");
    case ShuffleMode::Library:
        return QStringLiteral("library");
    case ShuffleMode::Off:
        break;
    }
    return QStringLiteral("off");
}

void insertString(QJsonObject &object, const QString &key, const QString &value)
{
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty()) {
        object.insert(key, trimmed);
    }
}

void insertPositive(QJsonObject &object, const QString &key, qint64 value)
{
    if (value > 0) {
        object.insert(key, value);
    }
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
    QString LoopStatus() const { return m_service->loopStatus(); }
    void SetLoopStatus(const QString &status)
    {
        if (status == QStringLiteral("Track")) {
            emit m_service->repeatModeRequested(RepeatMode::One);
        } else if (status == QStringLiteral("Playlist")) {
            emit m_service->repeatModeRequested(RepeatMode::All);
        } else {
            emit m_service->repeatModeRequested(RepeatMode::Off);
        }
    }
    double Rate() const { return 1.0; }
    void SetRate(double) {}
    bool Shuffle() const { return m_service->shuffle(); }
    void SetShuffle(bool on) { emit m_service->shuffleModeRequested(on ? ShuffleMode::Queue : ShuffleMode::Off); }
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

class MuzaitenPlayerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.muzaiten.Player")
    Q_PROPERTY(QString CurrentTrackJson READ CurrentTrackJson)

public:
    explicit MuzaitenPlayerAdaptor(MprisService *service)
        : QDBusAbstractAdaptor(service)
        , m_service(service)
    {
        setAutoRelaySignals(true);
    }

    QString CurrentTrackJson() const { return m_service->currentTrackJson(); }

public slots:
    QString GetCurrentTrackJson() const { return m_service->currentTrackJson(); }

private:
    MprisService *m_service = nullptr;
};

} // namespace

MprisService::MprisService(QObject *parent)
    : QObject(parent)
{
    new MprisRootAdaptor(this);
    new MprisPlayerAdaptor(this);
    new MuzaitenPlayerAdaptor(this);

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

QString MprisService::currentTrackJson() const
{
    return buildCurrentTrackJson(m_track);
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
    // Reset unconditionally: when the track is cleared ({} on queue-clear /
    // end-of-queue) or switched to one whose duration is not yet known, keeping
    // the previous value left a stale mpris:length and CanSeek=true exposed to
    // MPRIS clients.
    m_durationMs = track.durationMs > 0 ? track.durationMs : 0;
    emitPropertiesChanged(QString::fromLatin1(playerInterface),
                          {{QStringLiteral("Metadata"), metadata()},
                           {QStringLiteral("CanSeek"), canSeek()},
                           {QStringLiteral("CanPlay"), canPlay()}});
    emitPropertiesChanged(QString::fromLatin1(muzaitenPlayerInterface),
                          {{QStringLiteral("CurrentTrackJson"), currentTrackJson()}});
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
    emitPropertiesChanged(QString::fromLatin1(muzaitenPlayerInterface),
                          {{QStringLiteral("CurrentTrackJson"), currentTrackJson()}});
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
    emitPropertiesChanged(QString::fromLatin1(muzaitenPlayerInterface),
                          {{QStringLiteral("CurrentTrackJson"), currentTrackJson()}});
}

void MprisService::setVolume(double volume0To1)
{
    const double volume = std::clamp(volume0To1, 0.0, 1.0);
    if (qFuzzyCompare(m_volume, volume)) {
        return;
    }
    m_volume = volume;
    emitPropertiesChanged(QString::fromLatin1(playerInterface), {{QStringLiteral("Volume"), m_volume}});
    emitPropertiesChanged(QString::fromLatin1(muzaitenPlayerInterface),
                          {{QStringLiteral("CurrentTrackJson"), currentTrackJson()}});
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
    emitPropertiesChanged(QString::fromLatin1(muzaitenPlayerInterface),
                          {{QStringLiteral("CurrentTrackJson"), currentTrackJson()}});
}

QString MprisService::loopStatus() const
{
    switch (m_repeatMode) {
    case RepeatMode::One:
        return QStringLiteral("Track");
    case RepeatMode::All:
        return QStringLiteral("Playlist");
    case RepeatMode::Off:
        break;
    }
    return QStringLiteral("None");
}

bool MprisService::shuffle() const
{
    return m_shuffleMode != ShuffleMode::Off;
}

void MprisService::setRepeatMode(RepeatMode mode)
{
    if (m_repeatMode == mode) {
        return;
    }
    m_repeatMode = mode;
    emitPropertiesChanged(QString::fromLatin1(playerInterface),
                          {{QStringLiteral("LoopStatus"), loopStatus()}});
    emitPropertiesChanged(QString::fromLatin1(muzaitenPlayerInterface),
                          {{QStringLiteral("CurrentTrackJson"), currentTrackJson()}});
}

void MprisService::setShuffleMode(ShuffleMode mode)
{
    if (m_shuffleMode == mode) {
        return;
    }
    const bool wasShuffling = m_shuffleMode != ShuffleMode::Off;
    m_shuffleMode = mode;
    // MPRIS exposes only a bool, so its Shuffle signal fires only when the
    // on/off state flips; the custom JSON distinguishes queue vs library, so it
    // refreshes on any mode change.
    if (wasShuffling != (m_shuffleMode != ShuffleMode::Off)) {
        emitPropertiesChanged(QString::fromLatin1(playerInterface),
                              {{QStringLiteral("Shuffle"), shuffle()}});
    }
    emitPropertiesChanged(QString::fromLatin1(muzaitenPlayerInterface),
                          {{QStringLiteral("CurrentTrackJson"), currentTrackJson()}});
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

QString MprisService::buildCurrentTrackJson(const Track &track) const
{
    const qint64 durationMs = track.durationMs > 0 ? track.durationMs : m_durationMs;
    const qint64 elapsedMs = std::clamp(m_positionMs, qint64{0}, std::max<qint64>(0, durationMs));
    const double duration = round2(secondsFromMs(durationMs));
    const double elapsed = round2(secondsFromMs(elapsedMs));
    const double elapsedPercent = round2(duration > 0.0 ? (elapsed / duration) * 100.0 : 0.0);

    QJsonObject player;
    player.insert(QStringLiteral("name"), QStringLiteral("muzaiten"));
    player.insert(QStringLiteral("identity"), QStringLiteral(MUZAITEN_APP_NAME));
    player.insert(QStringLiteral("source"), QString::fromLatin1(muzaitenPlayerInterface));

    QJsonObject playback;
    playback.insert(QStringLiteral("state"), stateName(m_state));
    playback.insert(QStringLiteral("duration"), duration);
    playback.insert(QStringLiteral("elapsed"), elapsed);
    playback.insert(QStringLiteral("elapsed_percent"), elapsedPercent);
    playback.insert(QStringLiteral("volume"), static_cast<int>(std::lround(m_volume * 100.0)));
    playback.insert(QStringLiteral("repeat"), repeatModeName(m_repeatMode));
    playback.insert(QStringLiteral("shuffle"), shuffleModeName(m_shuffleMode));
    playback.insert(QStringLiteral("can_go_next"), m_canGoNext);
    playback.insert(QStringLiteral("can_go_previous"), m_canGoPrevious);
    playback.insert(QStringLiteral("can_play"), m_canPlay);
    playback.insert(QStringLiteral("can_pause"), canPause());
    playback.insert(QStringLiteral("can_seek"), canSeek());

    QJsonObject musicbrainz;
    insertString(musicbrainz, QStringLiteral("artist_id"), track.musicBrainz.artistId);
    insertString(musicbrainz, QStringLiteral("album_artist_id"), track.musicBrainz.albumArtistId);
    insertString(musicbrainz, QStringLiteral("release_id"), track.musicBrainz.releaseId);
    insertString(musicbrainz, QStringLiteral("release_group_id"), track.musicBrainz.releaseGroupId);
    insertString(musicbrainz, QStringLiteral("recording_id"), track.musicBrainz.recordingId);
    insertString(musicbrainz, QStringLiteral("track_id"), track.musicBrainz.trackId);
    insertString(musicbrainz, QStringLiteral("work_id"), track.musicBrainz.workId);

    QJsonObject trackObj;
    insertString(trackObj, QStringLiteral("title"), titleForTrack(track));
    insertString(trackObj, QStringLiteral("artist"), artistForTrack(track));
    insertString(trackObj, QStringLiteral("album_artist"), track.albumArtistName);
    insertString(trackObj, QStringLiteral("album"), track.albumTitle);
    insertString(trackObj, QStringLiteral("date"), track.date);
    insertString(trackObj, QStringLiteral("original_date"), track.originalDate);
    insertPositive(trackObj, QStringLiteral("track_number"), track.trackNumber);
    insertPositive(trackObj, QStringLiteral("track_total"), track.trackTotal);
    insertPositive(trackObj, QStringLiteral("disc_number"), track.discNumber);
    insertPositive(trackObj, QStringLiteral("disc_total"), track.discTotal);
    if (!musicbrainz.isEmpty()) {
        trackObj.insert(QStringLiteral("musicbrainz"), musicbrainz);
    }

    QJsonObject audio;
    insertString(audio, QStringLiteral("codec"), track.codec);
    insertPositive(audio, QStringLiteral("sample_rate_hz"), track.sampleRateHz);
    insertPositive(audio, QStringLiteral("bitrate_kbps"), track.bitrateKbps);
    insertPositive(audio, QStringLiteral("channels"), track.channels);
    insertPositive(audio, QStringLiteral("bit_depth"), track.bitDepth);

    QJsonObject library;
    if (track.rating0To100 >= 0) {
        library.insert(QStringLiteral("rating"), track.rating0To100);
        library.insert(QStringLiteral("rating_stars"), round2(track.rating0To100 / 20.0));
    }
    if (track.effectiveRating0To100 >= 0) {
        library.insert(QStringLiteral("effective_rating"), track.effectiveRating0To100);
        library.insert(QStringLiteral("effective_rating_stars"), round2(track.effectiveRating0To100 / 20.0));
    }
    library.insert(QStringLiteral("rating_source"), ratingSourceName(track.ratingSource));
    library.insert(QStringLiteral("has_user_rating"), track.hasUserRating);
    insertPositive(library, QStringLiteral("play_count"), track.playCount);

    QJsonObject file;
    insertString(file, QStringLiteral("path"), track.path);
    insertString(file, QStringLiteral("url"), track.path.isEmpty() ? QString() : QUrl::fromLocalFile(track.path).toString());
    insertString(file, QStringLiteral("parent_dir"), track.parentDir);
    insertString(file, QStringLiteral("name"), track.filename);
    insertPositive(file, QStringLiteral("size"), track.fileSize);
    insertPositive(file, QStringLiteral("mtime"), track.fileMtime);

    QJsonObject root;
    root.insert(QStringLiteral("player"), player);
    root.insert(QStringLiteral("playback"), playback);
    root.insert(QStringLiteral("track"), trackObj);
    root.insert(QStringLiteral("audio"), audio);
    root.insert(QStringLiteral("library"), library);
    root.insert(QStringLiteral("file"), file);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
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
