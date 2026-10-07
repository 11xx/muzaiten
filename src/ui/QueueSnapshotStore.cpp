#include "ui/QueueSnapshotStore.h"

#include "core/Rating.h"
#include "db/Database.h"
#include "db/PlaylistDatabase.h"
#include "db/SettingsStore.h"
#include "player/PlayerCore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace {
constexpr int kAutomaticSavedQueueLimit = 15;
QString newQueueIdentity()
{
    return QStringLiteral("queue:%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString normalizedQueueSourceKind(const QString &kind)
{
    if (kind == QStringLiteral("album") || kind == QStringLiteral("playlist")) {
        return kind;
    }
    return QStringLiteral("queue");
}

bool queueSnapshotIsRadio(const QJsonObject &snapshot)
{
    return snapshot.value(QStringLiteral("source")).toString().trimmed() == QStringLiteral("radio");
}

qint64 queueSnapshotSavedAt(const QJsonObject &snapshot)
{
    qint64 savedAt = snapshot.value(QStringLiteral("savedAt")).toVariant().toLongLong();
    if (savedAt <= 0) {
        savedAt = static_cast<qint64>(snapshot.value(QStringLiteral("savedAt")).toDouble(0));
    }
    return savedAt;
}

QString queueSnapshotKey(const QJsonObject &snapshot)
{
    return QStringLiteral("%1|%2|%3")
        .arg(snapshot.value(QStringLiteral("id")).toString(),
             QString::number(queueSnapshotSavedAt(snapshot)),
             snapshot.value(QStringLiteral("name")).toString());
}

QJsonArray queueBacklogFromRoot(const QJsonObject &root)
{
    QJsonArray backlog;
    QSet<QString> seenIds;
    const auto appendSnapshot = [&backlog, &seenIds](const QJsonObject &snapshot) {
        if (snapshot.isEmpty() || queueSnapshotIsRadio(snapshot)) {
            return;
        }
        const QString id = snapshot.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && seenIds.contains(id)) {
            return;
        }
        backlog.append(snapshot);
        if (!id.isEmpty()) {
            seenIds.insert(id);
        }
    };

    appendSnapshot(root.value(QStringLiteral("previous")).toObject());
    for (const QJsonValue &value : root.value(QStringLiteral("backlog")).toArray()) {
        appendSnapshot(value.toObject());
    }
    return backlog;
}

QJsonArray radioQueueBacklogFromRoot(const QJsonObject &root)
{
    QJsonArray backlog;
    QSet<QString> seenIds;
    const auto appendSnapshot = [&backlog, &seenIds](QJsonObject snapshot, bool forceRadio) {
        if (snapshot.isEmpty()) {
            return;
        }
        if (forceRadio) {
            snapshot.insert(QStringLiteral("source"), QStringLiteral("radio"));
        } else if (!queueSnapshotIsRadio(snapshot)) {
            return;
        }
        const QString id = snapshot.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && seenIds.contains(id)) {
            return;
        }
        backlog.append(snapshot);
        if (!id.isEmpty()) {
            seenIds.insert(id);
        }
    };

    for (const QJsonValue &value : root.value(QStringLiteral("radioBacklog")).toArray()) {
        appendSnapshot(value.toObject(), true);
    }
    appendSnapshot(root.value(QStringLiteral("previous")).toObject(), false);
    for (const QJsonValue &value : root.value(QStringLiteral("backlog")).toArray()) {
        appendSnapshot(value.toObject(), false);
    }
    return backlog;
}

QVector<QJsonObject> automaticQueueSnapshotsFromRoot(const QJsonObject &root)
{
    QVector<QJsonObject> snapshots;
    for (const QJsonValue &value : queueBacklogFromRoot(root)) {
        const QJsonObject snapshot = value.toObject();
        if (!snapshot.isEmpty()) {
            snapshots.push_back(snapshot);
        }
    }
    for (const QJsonValue &value : radioQueueBacklogFromRoot(root)) {
        const QJsonObject snapshot = value.toObject();
        if (!snapshot.isEmpty()) {
            snapshots.push_back(snapshot);
        }
    }
    std::stable_sort(snapshots.begin(), snapshots.end(), [](const QJsonObject &left, const QJsonObject &right) {
        return queueSnapshotSavedAt(left) > queueSnapshotSavedAt(right);
    });
    return snapshots;
}

QJsonObject trackToJson(const Track &track)
{
    QJsonObject root;
    root.insert(QStringLiteral("path"), track.path);
    root.insert(QStringLiteral("parentDir"), track.parentDir);
    root.insert(QStringLiteral("filename"), track.filename);
    root.insert(QStringLiteral("title"), track.title);
    root.insert(QStringLiteral("artistName"), track.artistName);
    root.insert(QStringLiteral("albumArtistName"), track.albumArtistName);
    root.insert(QStringLiteral("albumTitle"), track.albumTitle);
    root.insert(QStringLiteral("date"), track.date);
    root.insert(QStringLiteral("originalDate"), track.originalDate);
    root.insert(QStringLiteral("trackNumber"), track.trackNumber);
    root.insert(QStringLiteral("discNumber"), track.discNumber);
    root.insert(QStringLiteral("durationMs"), QString::number(track.durationMs));
    root.insert(QStringLiteral("rating0To100"), track.rating0To100);
    root.insert(QStringLiteral("effectiveRating0To100"), track.effectiveRating0To100);
    root.insert(QStringLiteral("hasUserRating"), track.hasUserRating);
    root.insert(QStringLiteral("fileSize"), QString::number(track.fileSize));
    root.insert(QStringLiteral("missing"), track.missing);
    // Technical fields the playback path needs — codec especially, since the DSD
    // output strategy (native vs PCM) keys off it. Without these a restored queue
    // would lose its codec and silently fall back to PCM for DSD.
    root.insert(QStringLiteral("codec"), track.codec);
    root.insert(QStringLiteral("sampleRateHz"), track.sampleRateHz);
    root.insert(QStringLiteral("bitrateKbps"), track.bitrateKbps);
    root.insert(QStringLiteral("channels"), track.channels);
    root.insert(QStringLiteral("bitDepth"), track.bitDepth);
    return root;
}

Track trackFromJson(const QJsonObject &root)
{
    Track track;
    track.path = root.value(QStringLiteral("path")).toString();
    track.parentDir = root.value(QStringLiteral("parentDir")).toString();
    track.filename = root.value(QStringLiteral("filename")).toString(QFileInfo(track.path).fileName());
    track.title = root.value(QStringLiteral("title")).toString();
    track.artistName = root.value(QStringLiteral("artistName")).toString();
    track.albumArtistName = root.value(QStringLiteral("albumArtistName")).toString();
    track.albumTitle = root.value(QStringLiteral("albumTitle")).toString();
    track.date = root.value(QStringLiteral("date")).toString();
    track.originalDate = root.value(QStringLiteral("originalDate")).toString();
    track.trackNumber = root.value(QStringLiteral("trackNumber")).toInt();
    track.discNumber = root.value(QStringLiteral("discNumber")).toInt();
    track.durationMs = root.value(QStringLiteral("durationMs")).toString().toLongLong();
    track.rating0To100 = root.value(QStringLiteral("rating0To100")).toInt(Rating::unset);
    track.effectiveRating0To100 = root.value(QStringLiteral("effectiveRating0To100")).toInt(track.rating0To100);
    track.hasUserRating = root.value(QStringLiteral("hasUserRating")).toBool();
    track.fileSize = root.value(QStringLiteral("fileSize")).toString().toLongLong();
    track.missing = root.value(QStringLiteral("missing")).toBool(false);
    track.codec = root.value(QStringLiteral("codec")).toString();
    // Snapshots written before codec was persisted (and any other gap) fall back
    // to the lower-cased file extension, which is exactly what the scanner stores
    // — so DSD detection recovers immediately even for an old saved queue.
    if (track.codec.isEmpty()) {
        track.codec = QFileInfo(track.path).suffix().toLower();
    }
    track.sampleRateHz = root.value(QStringLiteral("sampleRateHz")).toInt();
    track.bitrateKbps = root.value(QStringLiteral("bitrateKbps")).toInt();
    track.channels = root.value(QStringLiteral("channels")).toInt();
    track.bitDepth = root.value(QStringLiteral("bitDepth")).toInt();
    return track;
}
}

QueueSnapshotStore::QueueSnapshotStore(PlayerCore &player, Database &database, PlaylistDatabase &playlists,
                                       SettingsStore &state, QObject *parent)
    : QObject(parent), m_player(player), m_database(database), m_playlists(playlists), m_state(state),
      m_saveTimer(new QTimer(this))
{
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500);
    connect(m_saveTimer, &QTimer::timeout, this, &QueueSnapshotStore::saveQueueState);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &QueueSnapshotStore::saveQueueState);
    connect(&m_player, &PlayerCore::queueChanged, this, &QueueSnapshotStore::syncQueueIdentity);
    connect(&m_player, &PlayerCore::queueReset, this, &QueueSnapshotStore::syncQueueIdentity);
    connect(&m_player, &PlayerCore::currentIndexChanged, this, [this]() { scheduleQueueStateSave(); });
    connect(&m_player, &PlayerCore::playNextRangeChanged, this, [this]() { scheduleQueueStateSave(); });
    connect(&m_player, &PlayerCore::queueTracksChanged, this, [this]() { scheduleQueueStateSave(); });
    connect(&m_player, &PlayerCore::aboutToAddTracks, this, &QueueSnapshotStore::prepareQueueForTrackAddition);
    connect(&m_player, &PlayerCore::aboutToInjectLibraryTrack, this, &QueueSnapshotStore::prepareQueueForLibraryInjection);
    connect(&m_player, &PlayerCore::radioActiveChanged, this, [this](bool active) {
        if (active) markQueueAsSpontaneous();
    });
}

// The persisted queue lives in two settings. `queue.state` holds the tracks and
// the queue identity and is rewritten only when this fingerprint changes;
// `queue.cursor` holds the index and play-next boundary, tagged with the queue
// id, and is written on every save. The fingerprint covers what the structure
// document carries and the load side cannot recover from the library database:
// the identity fields, the track order, and the per-track fields that change in
// place (ratings, the missing flag).
QByteArray QueueSnapshotStore::currentQueueStructureFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto addString = [&hash](QStringView value) {
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(value.utf16()),
                                    value.size() * static_cast<qsizetype>(sizeof(char16_t))));
        hash.addData(QByteArrayView("\0", 1));
    };
    const auto addInt = [&hash](qint64 value) {
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(&value), sizeof(value)));
    };
    addString(m_queueId);
    addString(m_queueSourceKind);
    addInt(m_queueSourcePlaylistId);
    addString(m_queueSourceName);
    const QVector<Track> &queue = m_player.queue();
    addInt(queue.size());
    for (const Track &track : queue) {
        addString(track.path);
        addInt(track.rating0To100);
        addInt(track.effectiveRating0To100);
        addInt((track.hasUserRating ? 2 : 0) | (track.missing ? 1 : 0));
    }
    return hash.result();
}

void QueueSnapshotStore::loadQueueState()
{
    if (m_restored) return;
    m_restored = true;
    m_savedStructureFingerprint.clear();
    const QJsonObject root = QJsonDocument::fromJson(m_state.setting(QStringLiteral("queue.state")).toUtf8()).object();
    const QJsonArray trackValues = root.value(QStringLiteral("tracks")).toArray();
    QVector<Track> tracks;
    tracks.reserve(trackValues.size());
    for (const QJsonValue &value : trackValues) {
        Track track = trackFromJson(value.toObject());
        if (!track.path.isEmpty()) {
            const Track refreshed = m_database.trackForPath(track.path);
            if (!refreshed.path.isEmpty()) {
                track = refreshed;
            }
            tracks.push_back(track);
        }
    }

    // The cursor comes from `queue.cursor` when it belongs to this queue, else
    // from the cursor fields a structure document may carry. resetQueue() clamps
    // both fields to the loaded track count.
    const QString queueId = root.value(QStringLiteral("queueId")).toString();
    const QJsonObject cursor = QJsonDocument::fromJson(m_state.setting(QStringLiteral("queue.cursor")).toUtf8()).object();
    const bool cursorMatches = !queueId.isEmpty() && cursor.value(QStringLiteral("queueId")).toString() == queueId;
    const QJsonObject &cursorSource = cursorMatches ? cursor : root;
    const int savedIndex = cursorSource.value(QStringLiteral("index")).toInt(-1);
    m_queueId = queueId;
    m_queueSourceKind = normalizedQueueSourceKind(root.value(QStringLiteral("queueSourceKind")).toString(QStringLiteral("queue")));
    m_queueSourcePlaylistId = root.value(QStringLiteral("queueSourcePlaylistId")).toString().toLongLong();
    if (m_queueSourcePlaylistId <= 0) {
        m_queueSourcePlaylistId = static_cast<qint64>(root.value(QStringLiteral("queueSourcePlaylistId")).toDouble(0));
    }
    m_queueSourceName = root.value(QStringLiteral("queueSourceName")).toString();
    m_player.resetQueue(tracks, savedIndex,
                         cursorSource.value(QStringLiteral("playNextInsertIndex")).toInt(savedIndex + 1));
}

void QueueSnapshotStore::saveQueueState()
{
    m_saveTimer->stop();
    if (!m_player.queue().isEmpty()) {
        ensureCurrentQueueIdentity();
    }
    // The structure is written before the cursor: a crash between the two
    // leaves a cursor at most one save stale, and the load side clamps it.
    const QByteArray fingerprint = currentQueueStructureFingerprint();
    if (fingerprint != m_savedStructureFingerprint) {
        QJsonArray tracks;
        for (const Track &track : m_player.queue()) {
            tracks.append(trackToJson(track));
        }
        QJsonObject root;
        root.insert(QStringLiteral("tracks"), tracks);
        root.insert(QStringLiteral("queueId"), m_queueId);
        root.insert(QStringLiteral("queueSourceKind"), m_queueSourceKind);
        root.insert(QStringLiteral("queueSourcePlaylistId"), QString::number(m_queueSourcePlaylistId));
        root.insert(QStringLiteral("queueSourceName"), m_queueSourceName);
        if (m_state.setSetting(QStringLiteral("queue.state"),
                                         QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)))) {
            m_savedStructureFingerprint = fingerprint;
        }
    }

    QJsonObject cursor;
    cursor.insert(QStringLiteral("queueId"), m_queueId);
    cursor.insert(QStringLiteral("index"), m_player.queueIndex());
    cursor.insert(QStringLiteral("playNextInsertIndex"), m_player.playNextInsertIndex());
    m_state.setSetting(QStringLiteral("queue.cursor"), QString::fromUtf8(QJsonDocument(cursor).toJson(QJsonDocument::Compact)));
}

void QueueSnapshotStore::scheduleQueueStateSave(bool immediate)
{
    if (immediate) {
        saveQueueState();
        return;
    }
    m_saveTimer->start();
}

QJsonObject QueueSnapshotStore::queueSnapshotObject(const QString &name, const QString &source) const
{
    QJsonArray tracks;
    for (const Track &track : m_player.queue()) {
        tracks.append(trackToJson(track));
    }
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("id"), m_queueId);
    snapshot.insert(QStringLiteral("name"), name);
    snapshot.insert(QStringLiteral("savedAt"), QDateTime::currentSecsSinceEpoch());
    snapshot.insert(QStringLiteral("index"), m_player.queueIndex());
    snapshot.insert(QStringLiteral("playNextInsertIndex"), m_player.playNextInsertIndex());
    snapshot.insert(QStringLiteral("sourceKind"), m_queueSourceKind);
    snapshot.insert(QStringLiteral("sourcePlaylistId"), QString::number(m_queueSourcePlaylistId));
    snapshot.insert(QStringLiteral("sourceName"), m_queueSourceName);
    const QString trimmedSource = source.trimmed();
    if (!trimmedSource.isEmpty()) {
        snapshot.insert(QStringLiteral("source"), trimmedSource);
    }
    snapshot.insert(QStringLiteral("tracks"), tracks);
    return snapshot;
}

QJsonObject QueueSnapshotStore::loadQueueSnapshotsRoot() const
{
    return QJsonDocument::fromJson(m_state.setting(QStringLiteral("queue.snapshots")).toUtf8()).object();
}

void QueueSnapshotStore::saveQueueSnapshotsRoot(const QJsonObject &root)
{
    m_state.setSetting(QStringLiteral("queue.snapshots"),
                        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    emit snapshotsChanged();
}

QJsonObject QueueSnapshotStore::queueSnapshotByKey(const QString &keyOrId) const
{
    if (keyOrId.isEmpty()) {
        return {};
    }
    const QJsonObject root = loadQueueSnapshotsRoot();
    for (const QJsonObject &snapshot : automaticQueueSnapshotsFromRoot(root)) {
        if (queueSnapshotKey(snapshot) == keyOrId || snapshot.value(QStringLiteral("id")).toString() == keyOrId) {
            return snapshot;
        }
    }
    for (const QJsonValue &value : root.value(QStringLiteral("saved")).toArray()) {
        const QJsonObject snapshot = value.toObject();
        if (queueSnapshotKey(snapshot) == keyOrId || snapshot.value(QStringLiteral("id")).toString() == keyOrId) {
            return snapshot;
        }
    }
    return {};
}

int QueueSnapshotStore::savedQueueLimitSetting() const
{
    return savedQueueUnlimitedSetting() ? 0 : kAutomaticSavedQueueLimit;
}

int QueueSnapshotStore::radioSavedQueueLimitSetting() const
{
    return radioSavedQueueUnlimitedSetting() ? 0 : kAutomaticSavedQueueLimit;
}

bool QueueSnapshotStore::savedQueueUnlimitedSetting() const
{
    const QString value = m_state.setting(QStringLiteral("queue.savedQueueUnlimited")).trimmed();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

bool QueueSnapshotStore::radioSavedQueueUnlimitedSetting() const
{
    const QString value = m_state.setting(QStringLiteral("queue.radioSavedQueueUnlimited")).trimmed();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

void QueueSnapshotStore::ensureCurrentQueueIdentity()
{
    if (m_queueId.isEmpty()) {
        m_queueId = newQueueIdentity();
    }
    m_queueSourceKind = normalizedQueueSourceKind(m_queueSourceKind);
    if (m_queueSourceKind != QStringLiteral("playlist")) {
        m_queueSourcePlaylistId = 0;
    }
}

bool QueueSnapshotStore::currentQueueBacklogEligible() const
{
    return !m_player.queue().isEmpty() && m_queueSourceKind == QStringLiteral("queue");
}

QJsonObject QueueSnapshotStore::captureCurrentQueueSnapshot(const QString &source)
{
    if (!currentQueueBacklogEligible()) {
        return {};
    }
    QJsonObject snapshot = queueSnapshotObject(QString(), source);
    if (snapshot.value(QStringLiteral("id")).toString().isEmpty()) {
        snapshot.insert(QStringLiteral("id"), newQueueIdentity());
    }
    return snapshot;
}

void QueueSnapshotStore::pushQueueSnapshotToBacklog(const QJsonObject &snapshot)
{
    if (snapshot.isEmpty()) {
        return;
    }

    QJsonObject root = loadQueueSnapshotsRoot();
    const QString snapshotId = snapshot.value(QStringLiteral("id")).toString();
    QJsonArray backlog = queueBacklogFromRoot(root);
    QJsonArray radioBacklog = radioQueueBacklogFromRoot(root);
    const bool radioSnapshot = queueSnapshotIsRadio(snapshot);
    const int limit = radioSnapshot ? radioSavedQueueLimitSetting() : savedQueueLimitSetting();
    QJsonArray &targetBacklog = radioSnapshot ? radioBacklog : backlog;
    QJsonArray updatedBacklog;
    updatedBacklog.append(snapshot);
    for (const QJsonValue &value : targetBacklog) {
        const QJsonObject candidate = value.toObject();
        if (candidate.isEmpty() || (!snapshotId.isEmpty()
                                    && candidate.value(QStringLiteral("id")).toString() == snapshotId)) {
            continue;
        }
        updatedBacklog.append(candidate);
        if (limit > 0 && updatedBacklog.size() >= limit) {
            break;
        }
    }
    targetBacklog = updatedBacklog;
    root.remove(QStringLiteral("previous"));
    root.insert(QStringLiteral("backlog"), backlog);
    root.insert(QStringLiteral("radioBacklog"), radioBacklog);
    saveQueueSnapshotsRoot(root);
}

void QueueSnapshotStore::pushCurrentQueueToBacklog(const QString &name, const QString &source)
{
    if (!currentQueueBacklogEligible()) {
        return;
    }
    ensureCurrentQueueIdentity();
    pushQueueSnapshotToBacklog(queueSnapshotObject(name, source));
}

void QueueSnapshotStore::snapshotCurrentQueueAsPrevious(const QString &source)
{
    pushCurrentQueueToBacklog(QString(), source);
}

void QueueSnapshotStore::markQueueAsSpontaneous(const QString &id)
{
    setQueueIdentity(id.isEmpty() ? newQueueIdentity() : id);
}

void QueueSnapshotStore::setQueueIdentity(const QString &id, const QString &kind,
                                          qint64 playlistId, const QString &name)
{
    m_queueId = id;
    m_queueSourceKind = normalizedQueueSourceKind(kind);
    m_queueSourcePlaylistId = m_queueSourceKind == QStringLiteral("playlist") ? playlistId : 0;
    m_queueSourceName = name;
    emit sourceChanged();
    scheduleQueueStateSave();
}

void QueueSnapshotStore::syncQueueIdentity()
{
    if (m_player.queue().isEmpty()) {
        setQueueIdentity({});
    } else {
        ensureCurrentQueueIdentity();
    }
    scheduleQueueStateSave();
}

void QueueSnapshotStore::prepareQueueForTrackAddition(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) return;
    if (m_queueSourceKind == QStringLiteral("playlist")) {
        if (m_suppressPlaylistMirror || m_queueSourcePlaylistId <= 0) return;
        QSet<QString> existingPaths;
        for (const PlaylistItem &item : m_playlists.items(m_queueSourcePlaylistId)) {
            if (!item.trackPath.isEmpty()) existingPaths.insert(item.trackPath);
        }
        bool changed = false;
        for (const Track &track : tracks) {
            if (track.path.isEmpty() || existingPaths.contains(track.path)) continue;
            PlaylistItem item;
            item.trackPath = track.path;
            item.titleSnapshot = track.title.isEmpty() ? track.filename : track.title;
            item.artistSnapshot = track.artistName.isEmpty() ? track.albumArtistName : track.artistName;
            item.albumSnapshot = track.albumTitle;
            item.durationMs = track.durationMs;
            item.status = PlaylistItemStatus::Matched;
            item.effectiveRating0To100 = track.effectiveRating0To100;
            changed = m_playlists.addItem(m_queueSourcePlaylistId, item) > 0 || changed;
            existingPaths.insert(track.path);
        }
        if (changed) emit playlistItemsChanged();
    } else if (m_queueSourceKind == QStringLiteral("album")) {
        markQueueAsSpontaneous();
    } else {
        ensureCurrentQueueIdentity();
    }
}

void QueueSnapshotStore::prepareQueueForLibraryInjection(const Track &track)
{
    // Player-selected library/radio tracks stay queue-only in a playlist session.
    if (track.path.isEmpty() || m_queueSourceKind == QStringLiteral("playlist")) return;
    if (m_queueSourceKind == QStringLiteral("album")) {
        markQueueAsSpontaneous();
    } else {
        ensureCurrentQueueIdentity();
    }
}
