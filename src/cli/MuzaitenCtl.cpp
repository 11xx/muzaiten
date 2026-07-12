// muzaitenctl — command-line client for the muzaiten IPC socket.
//
// Talks newline-delimited JSON to the IpcServer inside a running muzaiten
// instance (same state-root resolution rules, so MUZAITEN_* env vars select
// which instance). Designed for scripting and compositor keybinds:
//   muzaitenctl rate 4 && notify-send "rated $(muzaitenctl status --format artist-title)"

#include "cli/SearchCli.h"
#include "app/AppPaths.h"
#include "core/GenreTags.h"
#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "features/FeatureStore.h"
#include "features/QueryEmbedding.h"
#include "features/QueryVectorCache.h"
#include "features/QualityRank.h"
#include "ipc/IpcSocket.h"
#include "reco/GenreCuration.h"
#include "reco/TrackScorer.h"
#include "reco/WeightLearner.h"
#include "reco/WeightLearnerData.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include <unistd.h>

namespace {

constexpr int connectTimeoutMs = 2000;
constexpr int replyTimeoutMs = 5000;
constexpr int semanticQueryTimeoutMs = 10 * 60 * 1000;
constexpr int defaultSemanticSearchLimit = 20;

void printUsage()
{
    std::fputs(
        "Usage: muzaitenctl [--json] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status                  show the current track and player state\n"
        "  play | pause | stop     transport control\n"
        "  play-pause | toggle     toggle playback\n"
        "  next | prev             queue navigation\n"
        "  seek <pos>              absolute seconds or mm:ss; +N / -N seeks relative\n"
        "  volume <pct>            absolute 0-100; +N / -N adjusts relative\n"
        "  rate <stars>            rate the current track: 0-5 (halves allowed)\n"
        "  rate raw <0-100>        rate on the raw 0-100 scale\n"
        "  rate clear              remove the user rating\n"
        "  queue                   list the queue (current row marked with >)\n"
        "  queue <index> | jump <index>  play the given queue row\n"
        "  search [opts] [text]    fold-aware library search (TSV; works offline)\n"
        "                            with no text on a terminal: fzf picker; piped: full dump\n"
        "      --plain               human-readable block instead of TSV\n"
        "      --limit N             cap the number of results\n"
        "      --fuzzy               fuzzy match instead of exact substring\n"
        "      --refresh             rebuild the on-disk cache from the library\n"
        "      --clear-cache         delete the cache and exit\n"
        "  semantic-search [--limit N] [--no-cache] <text>\n"
        "                          CLAP text-to-library search (requires muzaiten-features-clap)\n"
        "  genre-report [--plain]  dump folded genre vocabulary stats (works offline)\n"
        "  features-status         show features.sqlite coverage (works offline)\n"
        "  duplicate-groups [--min-size N]  inspect features.sqlite duplicate groups\n"
        "  pin-copy <group-id> <path> | unpin-copy <group-id>\n"
        "                          prefer one library copy for radio playback\n"
        "  radio-genre ignore <genre> | unignore <genre> | list\n"
        "                          curate radio-only ignored folded genres (works offline)\n"
        "  radio-weights get | set <json> | save <name> | apply <name> | list | remove <name>\n"
        "                          inspect and curate radio scoring weights (works offline)\n"
        "  radio-learn [--dry-run] [--min-samples N]\n"
        "                          suggest a learned radio-weight profile from local telemetry\n"
        "  genre-alias set <alias> <canonical> | remove <alias> | list\n"
        "                          curate folded genre aliases (works offline)\n"
        "  play-file <path>        append a file to the queue and play it\n"
        "  enqueue [--play|--next] <path...>  add files to the queue\n"
        "  raise                   show and focus the running instance's window\n"
        "  scrobble-backfill <listenbrainz|lastfm>  import listening history / sync play counts\n"
        "  scrobble-backfill status                 show progress of the current/last backfill\n"
        "  scrobble-backfill cancel                 cancel a running backfill (stops auto-resume)\n"
        "  scrobble-backfill reset <listenbrainz|lastfm>  clear the completed marker so the next run re-walks history\n"
        "  start-radio <path>      start a radio session seeded from a library track\n"
        "  start-artist-radio <artist>  start a radio session seeded from an artist\n"
        "  start-mix <mode>        start rediscovery or deepcuts radio mix\n"
        "  radio-reasons          show live radio pick explanations\n"
        "  stop-radio              stop the current radio session\n"
        "\n"
        "Options:\n"
        "  --json                  print the raw JSON reply\n",
        stderr);
}

QString formatSeconds(double seconds)
{
    const int total = static_cast<int>(std::lround(std::max(0.0, seconds)));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString formatSampleRate(int hz)
{
    if (hz <= 0) {
        return {};
    }
    QString khz = QString::number(hz / 1000.0, 'f', 1);
    if (khz.endsWith(QStringLiteral(".0"))) {
        khz.chop(2);
    }
    return khz + QStringLiteral("kHz");
}

QString starsText(int rating0To100)
{
    // Mirror the in-app convention: 20 points per star, halves shown as ½.
    const int halves = (rating0To100 + 5) / 10;
    QString out;
    for (int i = 0; i < halves / 2; ++i) {
        out += QStringLiteral("★");
    }
    if (halves % 2 != 0) {
        out += QStringLiteral("½");
    }
    return out.isEmpty() ? QStringLiteral("unrated") : out;
}

void printStatus(const QJsonObject &status)
{
    // Canonical track JSON: the player state lives under "playback", the tags
    // under "track", and ratings under "library".
    const QJsonObject playback = status.value(QStringLiteral("playback")).toObject();
    const QJsonObject track = status.value(QStringLiteral("track")).toObject();
    const QJsonObject library = status.value(QStringLiteral("library")).toObject();
    const QString title = track.value(QStringLiteral("title")).toString();
    const QString artist = track.value(QStringLiteral("artist")).toString();
    const QString album = track.value(QStringLiteral("album")).toString();

    if (title.isEmpty() && artist.isEmpty()) {
        std::printf("%s\n", qPrintable(playback.value(QStringLiteral("state")).toString(QStringLiteral("stopped"))));
        return;
    }
    std::printf("%s: %s - %s%s\n",
                qPrintable(playback.value(QStringLiteral("state")).toString()),
                qPrintable(artist.isEmpty() ? QStringLiteral("?") : artist),
                qPrintable(title),
                qPrintable(album.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(album)));
    std::printf("%s/%s  vol %d%%  rating %s\n",
                qPrintable(formatSeconds(playback.value(QStringLiteral("elapsed")).toDouble())),
                qPrintable(formatSeconds(playback.value(QStringLiteral("duration")).toDouble())),
                static_cast<int>(playback.value(QStringLiteral("volume")).toDouble()),
                qPrintable(starsText(library.value(QStringLiteral("effective_rating")).toInt(-1))));

    // Tech/quality line, when the scanned audio props are available (muzaiten
    // populates them; plain MPRIS sources leave them out).
    const QJsonObject audio = status.value(QStringLiteral("audio")).toObject();
    const QString codec = audio.value(QStringLiteral("codec")).toString();
    const int bitDepth = audio.value(QStringLiteral("bit_depth")).toInt();
    const int bitrate = audio.value(QStringLiteral("bitrate_kbps")).toInt();
    QStringList quality;
    if (!codec.isEmpty()) {
        quality << codec;
    }
    QString rateDepth = formatSampleRate(audio.value(QStringLiteral("sample_rate_hz")).toInt());
    if (bitDepth > 0) {
        rateDepth += (rateDepth.isEmpty() ? QString() : QStringLiteral("/")) + QStringLiteral("%1bit").arg(bitDepth);
    }
    if (!rateDepth.isEmpty()) {
        quality << rateDepth;
    }
    if (bitrate > 0) {
        quality << QStringLiteral("~%1kbps").arg(bitrate);
    }
    if (!quality.isEmpty()) {
        std::printf("%s\n", qPrintable(quality.join(QStringLiteral("  "))));
    }
}

// Returns -1 on parse failure. Accepts "90", "1:30"; a leading +/- marks the
// value relative (sign kept, `relative` set).
bool parseTime(QString text, double &seconds, bool &relative)
{
    relative = text.startsWith(QLatin1Char('+')) || text.startsWith(QLatin1Char('-'));
    const double sign = text.startsWith(QLatin1Char('-')) ? -1.0 : 1.0;
    if (relative) {
        text.remove(0, 1);
    }
    bool ok = false;
    if (text.contains(QLatin1Char(':'))) {
        const QStringList parts = text.split(QLatin1Char(':'));
        if (parts.size() != 2) {
            return false;
        }
        bool okMin = false;
        bool okSec = false;
        seconds = sign * (parts[0].toDouble(&okMin) * 60.0 + parts[1].toDouble(&okSec));
        return okMin && okSec;
    }
    seconds = sign * text.toDouble(&ok);
    return ok;
}

QString trackLine(const QJsonObject &track)
{
    const QString artist = track.value(QStringLiteral("artist")).toString();
    const QString album = track.value(QStringLiteral("album")).toString();
    QString line = QStringLiteral("%1 - %2").arg(artist.isEmpty() ? QStringLiteral("?") : artist,
                                                 track.value(QStringLiteral("title")).toString());
    if (!album.isEmpty()) {
        line += QStringLiteral(" [%1]").arg(album);
    }
    const double duration = track.value(QStringLiteral("duration")).toDouble();
    if (duration > 0) {
        line += QStringLiteral("  %1").arg(formatSeconds(duration));
    }
    return line;
}

int fail(const QString &message)
{
    std::fprintf(stderr, "muzaitenctl: %s\n", qPrintable(message));
    return 1;
}

// Tabs/newlines in a field would corrupt the TSV row layout; flatten them.
QString tsvField(const QString &value)
{
    QString v = value;
    v.replace(QLatin1Char('\t'), QLatin1Char(' '));
    v.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return v;
}

QString searchHumanLine(const Search::SearchRecord &r)
{
    const QString artist = r.artistName.isEmpty() ? QStringLiteral("?") : r.artistName;
    QString line = QStringLiteral("%1 - %2").arg(artist, r.title);
    if (!r.albumTitle.isEmpty()) {
        line += QStringLiteral(" [%1]").arg(r.albumTitle);
    }
    if (!r.date.isEmpty()) {
        line += QStringLiteral(" (%1)").arg(r.date);
    }
    if (r.durationMs > 0) {
        line += QStringLiteral("  %1").arg(formatSeconds(static_cast<double>(r.durationMs) / 1000.0));
    }
    return line;
}

QString featuresDbPath()
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("features.sqlite"));
}

QString historyDbPath()
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("history.sqlite"));
}

QJsonObject featureStatusJson(const QString &path, bool found, bool open, const FeatureStore::Status &status,
                              int schemaVersion, const QString &message = {})
{
    QJsonObject object{
        {QStringLiteral("path"), path},
        {QStringLiteral("found"), found},
        {QStringLiteral("open"), open},
        {QStringLiteral("files"), static_cast<double>(status.files)},
        {QStringLiteral("ok"), static_cast<double>(status.ok)},
        {QStringLiteral("failed"), static_cast<double>(status.failed)},
        {QStringLiteral("groups"), static_cast<double>(status.groups)},
        {QStringLiteral("featured"), static_cast<double>(status.featured)},
        {QStringLiteral("featured_fresh"), static_cast<double>(status.featuredFresh)},
        {QStringLiteral("featured_stale"), static_cast<double>(status.featuredStale)},
        {QStringLiteral("dsp_version"), status.dspVersion},
        {QStringLiteral("expected_dsp_version"), status.expectedDspVersion},
        {QStringLiteral("embedded_groups"), static_cast<double>(status.embeddedGroups)},
        {QStringLiteral("embedding_model"), status.embeddingModel},
        {QStringLiteral("embedding_version"), status.embeddingVersion},
        {QStringLiteral("neighbor_rows"), static_cast<double>(status.neighborRows)},
    };
    if (schemaVersion > 0) {
        object.insert(QStringLiteral("schema_version"), schemaVersion);
    }
    if (!message.isEmpty()) {
        object.insert(QStringLiteral("message"), message);
    }
    return object;
}

QStringList mediaTagsForPath(const Database &db, const QString &path)
{
    QStringList media;
    const MetadataBlob::FullMetadata metadata = db.fullMetadata(path);
    for (auto it = metadata.tags.cbegin(); it != metadata.tags.cend(); ++it) {
        if (it.key().compare(QStringLiteral("MEDIA"), Qt::CaseInsensitive) == 0) {
            media.append(it.value());
        }
    }
    return media;
}

QualityRank::Copy qualityCopyForTrack(const Track &track, const QStringList &mediaTags)
{
    return QualityRank::Copy{
        track.path,
        track.codec,
        track.bitDepth,
        track.sampleRateHz,
        track.bitrateKbps,
        mediaTags,
    };
}

struct DuplicateMember {
    Track track;
    QStringList mediaTags;
    int qualityScore = 0;
    bool pinned = false;
};

QVector<DuplicateMember> duplicateMembers(Database &db, const FeatureStore &features,
                                          qint64 groupId, const QString &pinnedPath)
{
    QVector<DuplicateMember> members;
    const QStringList paths = features.pathsInGroup(groupId);
    members.reserve(paths.size());
    for (const QString &path : paths) {
        Track track = db.trackForPath(path);
        if (track.path.isEmpty()) {
            continue;
        }
        db.enrichTrackForStatus(track);
        const QStringList mediaTags = mediaTagsForPath(db, path);
        const QualityRank::Copy copy = qualityCopyForTrack(track, mediaTags);
        members.push_back({track, mediaTags, QualityRank::score(copy), path == pinnedPath});
    }
    std::sort(members.begin(), members.end(), [](const DuplicateMember &left, const DuplicateMember &right) {
        if (left.pinned != right.pinned) {
            return left.pinned;
        }
        if (left.qualityScore != right.qualityScore) {
            return left.qualityScore > right.qualityScore;
        }
        return left.track.path < right.track.path;
    });
    return members;
}

QJsonObject duplicateMemberJson(const DuplicateMember &member)
{
    return QJsonObject{
        {QStringLiteral("path"), member.track.path},
        {QStringLiteral("codec"), member.track.codec},
        {QStringLiteral("bit_depth"), member.track.bitDepth},
        {QStringLiteral("sample_rate_hz"), member.track.sampleRateHz},
        {QStringLiteral("bitrate_kbps"), member.track.bitrateKbps},
        {QStringLiteral("quality_score"), member.qualityScore},
        {QStringLiteral("pinned"), member.pinned},
        {QStringLiteral("media"), QJsonArray::fromStringList(member.mediaTags)},
    };
}

struct SemanticScore {
    qint64 groupId = -1;
    double score = 0.0;
};

struct SemanticSearchResult {
    qint64 groupId = -1;
    double score = 0.0;
    DuplicateMember member;
};

double cosineSimilarity(const QVector<float> &left, const QVector<float> &right)
{
    if (left.size() != right.size() || left.isEmpty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double dot = 0.0;
    double leftNorm = 0.0;
    double rightNorm = 0.0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        const double a = left.at(i);
        const double b = right.at(i);
        if (!std::isfinite(a) || !std::isfinite(b)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        dot += a * b;
        leftNorm += a * a;
        rightNorm += b * b;
    }
    if (!(leftNorm > 0.0) || !(rightNorm > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return dot / (std::sqrt(leftNorm) * std::sqrt(rightNorm));
}

DuplicateMember bestCopyForGroup(Database &db, const FeatureStore &features,
                                 qint64 groupId, const QString &pinnedPath)
{
    const QVector<DuplicateMember> members = duplicateMembers(db, features, groupId, pinnedPath);
    if (members.isEmpty()) {
        return {};
    }

    QVector<QualityRank::Copy> copies;
    copies.reserve(members.size());
    for (const DuplicateMember &member : members) {
        copies.push_back(qualityCopyForTrack(member.track, member.mediaTags));
    }
    const QString bestPath = QualityRank::bestPath(copies, pinnedPath);
    for (const DuplicateMember &member : members) {
        if (member.track.path == bestPath) {
            return member;
        }
    }
    return members.first();
}

QVector<SemanticSearchResult> rankSemanticMatches(const QVector<float> &queryVector,
                                                  FeatureStore &features,
                                                  Database &db,
                                                  int limit,
                                                  QString *error)
{
    const QVector<qint64> groupIds = features.contentGroupIds(1);
    QList<qint64> groupList;
    groupList.reserve(groupIds.size());
    for (qint64 groupId : groupIds) {
        groupList.push_back(groupId);
    }

    const QHash<qint64, QVector<float>> embeddings = features.embeddingsForGroups(groupList);
    if (embeddings.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("features.sqlite has no active embeddings; run muzaiten-features refresh --semantic");
        }
        return {};
    }

    QVector<SemanticScore> scores;
    scores.reserve(embeddings.size());
    for (qint64 groupId : groupIds) {
        const QVector<float> embedding = embeddings.value(groupId);
        if (embedding.isEmpty() || embedding.size() != queryVector.size()) {
            continue;
        }
        const double score = cosineSimilarity(queryVector, embedding);
        if (std::isfinite(score)) {
            scores.push_back({groupId, score});
        }
    }
    if (scores.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("features.sqlite has no embeddings matching query dimension %1")
                         .arg(queryVector.size());
        }
        return {};
    }

    std::sort(scores.begin(), scores.end(), [](const SemanticScore &left, const SemanticScore &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.groupId < right.groupId;
    });

    const QHash<qint64, QString> pins = db.contentGroupPins();
    QVector<SemanticSearchResult> results;
    results.reserve(std::min(static_cast<qsizetype>(limit), scores.size()));
    // Each candidate group costs several library queries. When the stores
    // disagree (stale features.sqlite vs pruned library), no candidate ever
    // matches and an unbounded walk would turn one search into minutes of
    // full-store probing; a healthy store fills `limit` within the first
    // few candidates, so the cap only bites in the mismatched case.
    const qsizetype maxCandidates = std::max<qsizetype>(static_cast<qsizetype>(limit) * 20, 500);
    qsizetype examined = 0;
    for (const SemanticScore &score : scores) {
        if (++examined > maxCandidates) {
            break;
        }
        const DuplicateMember member = bestCopyForGroup(db, features, score.groupId, pins.value(score.groupId));
        if (member.track.path.isEmpty()) {
            continue;
        }
        results.push_back({score.groupId, score.score, member});
        if (results.size() >= limit) {
            break;
        }
    }
    if (results.isEmpty() && error != nullptr) {
        *error = QStringLiteral("semantic search found embeddings, but no matching groups are present in library.sqlite");
    }
    return results;
}

QJsonObject semanticResultJson(const SemanticSearchResult &result)
{
    QJsonObject object = duplicateMemberJson(result.member);
    object.insert(QStringLiteral("content_group_id"), static_cast<double>(result.groupId));
    object.insert(QStringLiteral("score"), result.score);
    object.insert(QStringLiteral("title"), result.member.track.title);
    object.insert(QStringLiteral("artist"), result.member.track.artistName);
    object.insert(QStringLiteral("album_artist"), result.member.track.albumArtistName);
    object.insert(QStringLiteral("album"), result.member.track.albumTitle);
    object.insert(QStringLiteral("date"), result.member.track.date);
    object.insert(QStringLiteral("duration"), std::round(static_cast<double>(result.member.track.durationMs) / 10.0) / 100.0);
    object.insert(QStringLiteral("rating"), result.member.track.rating0To100);
    return object;
}

QString qualitySummary(const DuplicateMember &member)
{
    QStringList parts;
    if (!member.track.codec.isEmpty()) {
        parts << member.track.codec;
    }
    if (member.track.bitDepth > 0 || member.track.sampleRateHz > 0) {
        QString rateDepth;
        if (member.track.bitDepth > 0) {
            rateDepth += QStringLiteral("%1bit").arg(member.track.bitDepth);
        }
        if (member.track.sampleRateHz > 0) {
            if (!rateDepth.isEmpty()) {
                rateDepth += QLatin1Char('/');
            }
            rateDepth += QStringLiteral("%1Hz").arg(member.track.sampleRateHz);
        }
        parts << rateDepth;
    }
    if (member.track.bitrateKbps > 0) {
        parts << QStringLiteral("%1kbps").arg(member.track.bitrateKbps);
    }
    if (!member.mediaTags.isEmpty()) {
        parts << QStringLiteral("MEDIA=%1").arg(member.mediaTags.join(QLatin1Char('|')));
    }
    parts << QStringLiteral("score=%1").arg(member.qualityScore);
    return parts.join(QLatin1Char(' '));
}

QString oneLine(const QString &value)
{
    QString v = value;
    v.replace(QLatin1Char('\n'), QLatin1Char(' '));
    v.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return v;
}

bool hasNonAscii(const QString &s)
{
    for (const QChar c : s) {
        if (c.unicode() > 127) {
            return true;
        }
    }
    return false;
}

QJsonArray genreRowsToJson(const QVector<GenreCuration::ReportRow> &rows)
{
    QJsonArray array;
    for (const GenreCuration::ReportRow &row : rows) {
        QJsonArray samples;
        for (const QString &artist : row.sampleArtists) {
            samples.append(artist);
        }
        QJsonArray flags;
        for (const QString &flag : row.flags) {
            flags.append(flag);
        }
        array.append(QJsonObject{
            {QStringLiteral("genre"), row.genre},
            {QStringLiteral("df"), row.df},
            {QStringLiteral("idf"), row.idf},
            {QStringLiteral("canonical"), row.canonical},
            {QStringLiteral("status"), row.status},
            {QStringLiteral("sample_artists"), samples},
            {QStringLiteral("flags"), flags},
        });
    }
    return array;
}

void printGenreReportTable(const QVector<GenreCuration::ReportRow> &rows, int taggedTrackTotal)
{
    int genreWidth = 5;
    int canonicalWidth = 9;
    int sampleWidth = 14;
    for (const GenreCuration::ReportRow &row : rows) {
        genreWidth = std::max(genreWidth, static_cast<int>(row.genre.size()));
        canonicalWidth = std::max(canonicalWidth, static_cast<int>(row.canonical.size()));
        sampleWidth = std::max(sampleWidth, static_cast<int>(row.sampleArtists.join(QStringLiteral(", ")).size()));
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << "genre-report: " << rows.size() << " genres, " << taggedTrackTotal << " tagged tracks\n";
    out << QStringLiteral("%1  %2  %3  %4  %5  %6  %7\n")
               .arg(QStringLiteral("genre"), -genreWidth)
               .arg(QStringLiteral("df"), 6)
               .arg(QStringLiteral("idf"), 6)
               .arg(QStringLiteral("canonical"), -canonicalWidth)
               .arg(QStringLiteral("status"), -8)
               .arg(QStringLiteral("sample_artists"), -sampleWidth)
               .arg(QStringLiteral("flags"));
    for (const GenreCuration::ReportRow &row : rows) {
        out << QStringLiteral("%1  %2  %3  %4  %5  %6  %7\n")
                   .arg(row.genre, -genreWidth)
                   .arg(row.df, 6)
                   .arg(QString::number(row.idf, 'f', 3), 6)
                   .arg(row.canonical, -canonicalWidth)
                   .arg(row.status, -8)
                   .arg(row.sampleArtists.join(QStringLiteral(", ")), -sampleWidth)
                   .arg(row.flags.join(QLatin1Char(',')));
    }
    out << "genre-report: end, " << rows.size() << " genres, " << taggedTrackTotal << " tagged tracks\n";
}

void printGenreReportTsv(const QVector<GenreCuration::ReportRow> &rows, int taggedTrackTotal)
{
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << "# vocabulary_size\t" << rows.size() << "\ttagged_track_total\t" << taggedTrackTotal << '\n';
    out << "genre\tdf\tidf\tcanonical\tstatus\tsample_artists\tflags\n";
    for (const GenreCuration::ReportRow &row : rows) {
        out << tsvField(row.genre) << '\t'
            << row.df << '\t'
            << QString::number(row.idf, 'f', 6) << '\t'
            << tsvField(row.canonical) << '\t'
            << row.status << '\t'
            << tsvField(row.sampleArtists.join(QStringLiteral(", "))) << '\t'
            << tsvField(row.flags.join(QLatin1Char(','))) << '\n';
    }
    out << "# end\tvocabulary_size\t" << rows.size() << "\ttagged_track_total\t" << taggedTrackTotal << '\n';
}

void printRadioReasons(const QJsonObject &response)
{
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    if (!response.value(QStringLiteral("active")).toBool()) {
        out << "radio: inactive\n";
        return;
    }

    const QString kind = response.value(QStringLiteral("kind")).toString(QStringLiteral("radio"));
    const QJsonArray picks = response.value(QStringLiteral("picks")).toArray();
    out << "radio: " << (kind.isEmpty() ? QStringLiteral("active") : kind)
        << " (" << picks.size() << " explained picks)\n";
    for (qsizetype i = 0; i < picks.size(); ++i) {
        const QJsonObject pick = picks.at(i).toObject();
        const QString path = pick.value(QStringLiteral("path")).toString();
        const QString artist = pick.value(QStringLiteral("artist")).toString();
        const QString title = pick.value(QStringLiteral("title")).toString();
        QString label = path;
        if (!artist.isEmpty() || !title.isEmpty()) {
            label = QStringLiteral("%1 - %2").arg(artist.isEmpty() ? QStringLiteral("?") : artist,
                                                  title.isEmpty() ? QStringLiteral("?") : title);
        }
        out << QStringLiteral("%1. %2\n").arg(static_cast<int>(i + 1), 3).arg(label);
        if (label != path && !path.isEmpty()) {
            out << "    " << path << '\n';
        }
        const QString sentence = pick.value(QStringLiteral("sentence")).toString();
        if (!sentence.isEmpty()) {
            out << "    " << sentence << '\n';
        }
        const QString breakdown = pick.value(QStringLiteral("breakdown")).toString();
        if (!breakdown.isEmpty()) {
            out << "    " << breakdown << '\n';
        }
    }
}

constexpr const char kRadioScoringWeightsKey[] = "radio.scoringWeights";

QJsonObject jsonObjectFromBytes(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

QString prettyJson(const QByteArray &json)
{
    const QJsonDocument document = QJsonDocument::fromJson(json);
    return QString::fromUtf8(document.toJson(QJsonDocument::Indented)).trimmed();
}

bool parseWeightsForCli(const QByteArray &json, TrackScorer::Weights *weights, QString *error)
{
    QString parseError;
    const TrackScorer::Weights parsed = TrackScorer::weightsFromJson(json, &parseError);
    if (!parseError.isEmpty()) {
        if (error != nullptr) {
            *error = parseError;
        }
        return false;
    }
    if (weights != nullptr) {
        *weights = parsed;
    }
    return true;
}

QByteArray compactJsonObject(const QByteArray &json)
{
    return QJsonDocument(QJsonDocument::fromJson(json).object()).toJson(QJsonDocument::Compact);
}

QByteArray activeWeightsJson(Database &db)
{
    return db.setting(QString::fromLatin1(kRadioScoringWeightsKey)).toUtf8();
}

QByteArray effectiveWeightsJson(const QByteArray &activeJson)
{
    QString error;
    const TrackScorer::Weights weights = TrackScorer::weightsFromJson(activeJson, &error);
    if (!error.isEmpty()) {
        return TrackScorer::weightsToJson(TrackScorer::defaultWeights());
    }
    return TrackScorer::weightsToJson(weights);
}

QJsonObject weightsDiffJson(const QByteArray &activeJson, const QByteArray &suggestedJson)
{
    const QJsonObject active = jsonObjectFromBytes(activeJson);
    const QJsonObject suggested = jsonObjectFromBytes(suggestedJson);
    QStringList keys = active.keys();
    for (const QString &key : suggested.keys()) {
        if (!keys.contains(key)) {
            keys.push_back(key);
        }
    }
    keys.sort(Qt::CaseInsensitive);

    QJsonObject diff;
    for (const QString &key : keys) {
        const QJsonValue activeValue = active.value(key);
        const QJsonValue suggestedValue = suggested.value(key);
        const bool bothNumeric = activeValue.isDouble() && suggestedValue.isDouble();
        const bool changed = bothNumeric
            ? std::abs(activeValue.toDouble() - suggestedValue.toDouble()) > 0.000001
            : activeValue != suggestedValue;
        if (changed) {
            diff.insert(key, QJsonObject{
                {QStringLiteral("active"), activeValue},
                {QStringLiteral("suggested"), suggestedValue},
            });
        }
    }
    return diff;
}

QJsonObject componentResultJson(const WeightLearner::ComponentResult &row,
                                const TrackScorer::Weights &activeWeights)
{
    double activeWeight = 0.0;
    WeightLearner::componentWeight(activeWeights, row.componentName, &activeWeight);
    return QJsonObject{
        {QStringLiteral("component"), row.componentName},
        {QStringLiteral("weight_key"), row.weightKey},
        {QStringLiteral("coefficient"), row.coefficient},
        {QStringLiteral("multiplier"), row.multiplier},
        {QStringLiteral("default_weight"), row.defaultWeight},
        {QStringLiteral("active_weight"), activeWeight},
        {QStringLiteral("suggested_weight"), row.suggestedWeight},
        {QStringLiteral("non_zero_samples"), row.nonZeroSamples},
    };
}

QJsonArray componentResultsJson(const QVector<WeightLearner::ComponentResult> &components,
                                const TrackScorer::Weights &activeWeights)
{
    QJsonArray array;
    for (const WeightLearner::ComponentResult &row : components) {
        array.append(componentResultJson(row, activeWeights));
    }
    return array;
}

void printRadioLearnPlain(const WeightLearner::Result &learned,
                          const TrackScorer::Weights &activeWeights,
                          const QByteArray &activeJson,
                          const WeightLearnerData::LoadResult &load,
                          const QString &profileName,
                          bool dryRun)
{
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << "radio-learn: " << learned.sampleCount << " labeled picks ("
        << learned.positiveLabels << " early skips)\n";
    out << "join window: " << WeightLearnerData::kJoinWindowSecs << " seconds\n";
    if (load.skippedInvalidWeights > 0 || load.skippedNoSignals > 0) {
        out << "skipped rows: " << load.skippedInvalidWeights << " invalid weights, "
            << load.skippedNoSignals << " without learnable components\n";
    }

    out << "component        multiplier  default   active    suggested  samples\n";
    for (const WeightLearner::ComponentResult &row : learned.components) {
        double activeWeight = 0.0;
        WeightLearner::componentWeight(activeWeights, row.componentName, &activeWeight);
        out << QStringLiteral("%1  %2  %3  %4  %5  %6\n")
                   .arg(row.componentName, -14)
                   .arg(QString::number(row.multiplier, 'f', 3), 10)
                   .arg(QString::number(row.defaultWeight, 'f', 3), 8)
                   .arg(QString::number(activeWeight, 'f', 3), 8)
                   .arg(QString::number(row.suggestedWeight, 'f', 3), 9)
                   .arg(row.nonZeroSamples, 7);
    }

    out << "suggested weights:\n" << prettyJson(learned.suggestedWeightsJson) << '\n';
    const QJsonObject diff = weightsDiffJson(activeJson, learned.suggestedWeightsJson);
    out << "diff vs active:\n";
    if (diff.isEmpty()) {
        out << "  (none)\n";
    } else {
        for (auto it = diff.constBegin(); it != diff.constEnd(); ++it) {
            const QJsonObject row = it.value().toObject();
            out << "  " << it.key() << ": "
                << QString::number(row.value(QStringLiteral("active")).toDouble(), 'f', 6)
                << " -> "
                << QString::number(row.value(QStringLiteral("suggested")).toDouble(), 'f', 6)
                << '\n';
        }
    }

    if (dryRun) {
        out << "radio-learn: dry run; no profile saved\n";
    } else {
        out << "radio-learn: saved profile " << profileName
            << "; apply later with radio-weights apply " << profileName << '\n';
    }
}

int runRadioLearn(QStringList arguments, bool json)
{
    bool dryRun = false;
    int minSamples = 200;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString word = arguments.at(i);
        if (word == QLatin1String("--dry-run")) {
            dryRun = true;
        } else if (word == QLatin1String("--min-samples")) {
            bool ok = false;
            if (i + 1 >= arguments.size() || (minSamples = arguments.at(++i).toInt(&ok)) <= 0 || !ok) {
                return fail(QStringLiteral("radio-learn --min-samples needs a positive number"));
            }
        } else if (word.startsWith(QLatin1String("--"))) {
            return fail(QStringLiteral("unknown radio-learn option \"%1\"").arg(word));
        } else {
            return fail(QStringLiteral("radio-learn does not take positional arguments"));
        }
    }

    if (!QFileInfo::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }
    if (!QFileInfo::exists(historyDbPath())) {
        return fail(QStringLiteral("history database not found at %1").arg(historyDbPath()));
    }

    Database db(QStringLiteral("muzaitenctl-radio-learn-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }

    const QByteArray activeJson = effectiveWeightsJson(activeWeightsJson(db));
    QString activeError;
    const TrackScorer::Weights activeWeights = TrackScorer::weightsFromJson(activeJson, &activeError);
    if (!activeError.isEmpty()) {
        return fail(activeError);
    }

    const WeightLearnerData::LoadResult load = WeightLearnerData::loadSamplesFromPath(historyDbPath());
    if (!load.error.isEmpty()) {
        return fail(load.error);
    }

    WeightLearner::Options options;
    options.minSamples = minSamples;
    options.minPositiveLabels = minSamples == 200 ? 20 : std::max(1, minSamples / 10);
    const WeightLearner::Result learned = WeightLearner::learn(load.samples, options);
    if (!learned.ok) {
        return fail(learned.error);
    }

    QString profileName;
    if (!dryRun) {
        profileName = QStringLiteral("learned-%1").arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
        if (!db.saveRadioWeightProfile(profileName, QString::fromUtf8(learned.suggestedWeightsJson))) {
            return fail(db.lastError());
        }
    }

    if (json) {
        QTextStream out(stdout);
        out.setEncoding(QStringConverter::Utf8);
        out << QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("dry_run"), dryRun},
            {QStringLiteral("profile"), profileName.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                              : QJsonValue(profileName)},
            {QStringLiteral("sample_count"), learned.sampleCount},
            {QStringLiteral("positive_labels"), learned.positiveLabels},
            {QStringLiteral("min_samples"), options.minSamples},
            {QStringLiteral("min_positive_labels"), options.minPositiveLabels},
            {QStringLiteral("join_window_seconds"), WeightLearnerData::kJoinWindowSecs},
            {QStringLiteral("skipped_invalid_weights"), load.skippedInvalidWeights},
            {QStringLiteral("skipped_no_signals"), load.skippedNoSignals},
            {QStringLiteral("components"), componentResultsJson(learned.components, activeWeights)},
            {QStringLiteral("active"), jsonObjectFromBytes(activeJson)},
            {QStringLiteral("suggested"), jsonObjectFromBytes(learned.suggestedWeightsJson)},
            {QStringLiteral("diff"), weightsDiffJson(activeJson, learned.suggestedWeightsJson)},
        }).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        printRadioLearnPlain(learned, activeWeights, activeJson, load, profileName, dryRun);
    }
    return 0;
}

int runRadioWeights(QStringList arguments, bool json)
{
    if (arguments.isEmpty()) {
        return fail(QStringLiteral("radio-weights needs a verb: get, set, save, apply, list, or remove"));
    }
    const QString verb = arguments.takeFirst();
    const QSet<QString> validVerbs{
        QStringLiteral("get"),
        QStringLiteral("set"),
        QStringLiteral("save"),
        QStringLiteral("apply"),
        QStringLiteral("list"),
        QStringLiteral("remove"),
    };
    if (!validVerbs.contains(verb)) {
        return fail(QStringLiteral("radio-weights verb must be get, set, save, apply, list, or remove"));
    }
    if ((verb == QLatin1String("get") || verb == QLatin1String("list")) && !arguments.isEmpty()) {
        return fail(QStringLiteral("radio-weights %1 does not take arguments").arg(verb));
    }
    if ((verb == QLatin1String("save") || verb == QLatin1String("apply") || verb == QLatin1String("remove"))
        && arguments.size() != 1) {
        return fail(QStringLiteral("radio-weights %1 needs exactly one profile name").arg(verb));
    }
    if (verb == QLatin1String("set") && arguments.size() != 1) {
        return fail(QStringLiteral("radio-weights set needs exactly one JSON object"));
    }
    if (!QFile::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }

    Database db(QStringLiteral("muzaitenctl-radio-weights-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    if (verb == QLatin1String("get")) {
        const QByteArray active = activeWeightsJson(db);
        const QByteArray effective = active.isEmpty()
            ? TrackScorer::weightsToJson(TrackScorer::defaultWeights())
            : effectiveWeightsJson(active);
        if (json) {
            out << QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("active"), active.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                            : QJsonValue(jsonObjectFromBytes(active))},
                {QStringLiteral("defaults"), active.isEmpty()},
                {QStringLiteral("effective"), jsonObjectFromBytes(effective)},
            }).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            out << "active:\n";
            out << (active.isEmpty() ? QStringLiteral("(defaults)") : prettyJson(active)) << '\n';
            out << "effective:\n" << prettyJson(effective) << '\n';
        }
        return 0;
    }

    if (verb == QLatin1String("list")) {
        const QVector<Database::RadioWeightProfile> profiles = db.radioWeightProfiles();
        if (json) {
            QJsonArray array;
            for (const Database::RadioWeightProfile &profile : profiles) {
                array.append(QJsonObject{
                    {QStringLiteral("name"), profile.name},
                    {QStringLiteral("weights"), jsonObjectFromBytes(profile.weightsJson.toUtf8())},
                    {QStringLiteral("updated_at"), profile.updatedAt},
                });
            }
            out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            for (const Database::RadioWeightProfile &profile : profiles) {
                out << profile.name << '\t' << profile.updatedAt << '\n';
            }
        }
        return 0;
    }

    if (verb == QLatin1String("set")) {
        const QByteArray raw = arguments.first().toUtf8();
        TrackScorer::Weights weights;
        QString error;
        if (!parseWeightsForCli(raw, &weights, &error)) {
            return fail(error);
        }
        const QByteArray compact = compactJsonObject(raw);
        if (!db.setSetting(QString::fromLatin1(kRadioScoringWeightsKey), QString::fromUtf8(compact))) {
            return fail(db.lastError());
        }
        if (json) {
            out << QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("action"), QStringLiteral("set")},
                {QStringLiteral("active"), jsonObjectFromBytes(compact)},
                {QStringLiteral("effective"), jsonObjectFromBytes(TrackScorer::weightsToJson(weights))},
                {QStringLiteral("takes_effect"), QStringLiteral("next radio session")},
            }).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            out << "radio-weights: set active weights; takes effect on next radio session\n";
        }
        return 0;
    }

    const QString profileName = arguments.first().trimmed();
    if (profileName.isEmpty()) {
        return fail(QStringLiteral("radio-weights %1 needs a non-empty profile name").arg(verb));
    }

    if (verb == QLatin1String("save")) {
        const QByteArray active = activeWeightsJson(db);
        const QByteArray snapshot = active.isEmpty()
            ? TrackScorer::weightsToJson(TrackScorer::defaultWeights())
            : effectiveWeightsJson(active);
        if (!db.saveRadioWeightProfile(profileName, QString::fromUtf8(snapshot))) {
            return fail(db.lastError());
        }
        if (json) {
            out << QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("action"), QStringLiteral("save")},
                {QStringLiteral("name"), profileName},
                {QStringLiteral("weights"), jsonObjectFromBytes(snapshot)},
            }).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            out << "radio-weights: saved profile " << profileName << '\n';
        }
        return 0;
    }

    if (verb == QLatin1String("apply")) {
        const QString profileJson = db.radioWeightProfile(profileName);
        if (profileJson.isEmpty()) {
            return fail(QStringLiteral("radio-weights profile not found: %1").arg(profileName));
        }
        if (!db.setSetting(QString::fromLatin1(kRadioScoringWeightsKey), profileJson)) {
            return fail(db.lastError());
        }
        if (json) {
            out << QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("action"), QStringLiteral("apply")},
                {QStringLiteral("name"), profileName},
                {QStringLiteral("active"), jsonObjectFromBytes(profileJson.toUtf8())},
                {QStringLiteral("takes_effect"), QStringLiteral("next radio session")},
            }).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            out << "radio-weights: applied profile " << profileName
                << "; takes effect on next radio session\n";
        }
        return 0;
    }

    if (!db.removeRadioWeightProfile(profileName)) {
        return fail(db.lastError());
    }
    if (json) {
        out << QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("action"), QStringLiteral("remove")},
            {QStringLiteral("name"), profileName},
        }).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        out << "radio-weights: removed profile " << profileName << '\n';
    }
    return 0;
}

// One NUL-terminated fzf record. Newline-separated lines:
//   1: clean path — hidden (--with-nth=2..), extracted by the queue/play actions
//   2..: pretty, colored display lines (artist/album/title/path)
//   last (only when the row has non-ASCII text): the folded romaji norm, dimmed
// fzf searches everything it displays, so the romaji line is what lets a romaji
// query match a kanji/accented row; for pure-ASCII rows the visible text already
// matches, so we omit it to keep them clean. Colors use the standard 16-color
// palette so they respect the terminal theme.
QByteArray pickerRecord(const Search::SearchRecord &r)
{
    const QString artist = oneLine(r.artistName.isEmpty() ? QStringLiteral("Unknown Artist") : r.artistName);
    const QString album  = oneLine(r.albumTitle.isEmpty() ? QStringLiteral("Unknown Album") : r.albumTitle);
    const QString title  = oneLine(r.title.isEmpty() ? r.filename : r.title);
    const QString path   = oneLine(r.path);

    // ♪ and ▸ render one column wide, 💿 and 📁 two — so the narrow ones get an
    // extra space to align the text column across all rows (cf. ndl's
    // align_prefixes). The dim reading line is indented to match.
    QString block = path + QLatin1Char('\n');                                 // line 1: hidden, for actions
    block += QStringLiteral("\033[36m♪  %1\033[0m\n").arg(artist);            // cyan
    block += QStringLiteral("\033[33m💿 %1\033[0m").arg(album);               // yellow
    if (!r.date.isEmpty()) {
        block += QStringLiteral(" \033[90m(%1)\033[0m").arg(oneLine(r.date));
    }
    block += QLatin1Char('\n');
    block += QStringLiteral("\033[32m▸  %1\033[0m").arg(title);               // green
    if (r.durationMs > 0) {
        block += QStringLiteral(" \033[90m[%1]\033[0m").arg(formatSeconds(static_cast<double>(r.durationMs) / 1000.0));
    }
    block += QLatin1Char('\n');
    block += QStringLiteral("\033[90m📁 %1\033[0m").arg(path);                // dim path (searchable)

    if (hasNonAscii(title + artist + album)) {
        const QString norm = oneLine(r.normTitle + QLatin1Char(' ') + r.normArtist + QLatin1Char(' ')
                                     + r.normAlbum);
        if (!norm.trimmed().isEmpty()) {
            block += QStringLiteral("\n\033[90m   %1\033[0m").arg(norm);      // dim romaji reading, aligned
        }
    }

    QByteArray bytes = block.toUtf8();
    bytes.append('\0');
    return bytes;
}

// Interactive fzf picker over the whole library. fzf does the fuzzy matching
// (against our pre-folded field), so romaji finds kanji; Return queues the
// selection, Alt-Return plays it — both via `muzaitenctl enqueue` over IPC.
// fzf is launched before the index loads and records are streamed in, so the
// TUI appears instantly and fills progressively.
int runPicker(const QString &fzfPath, bool fuzzy, bool refresh)
{
    if (!QFile::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }
    const QString self = QCoreApplication::applicationFilePath();
    // From the selected NUL records ({+f}), emit each path (line 1) NUL-separated
    // and pipe to `enqueue --stdin0`, so even thousands of marks fit (no ARG_MAX).
    const QString extract = QStringLiteral(
        "awk 'BEGIN{RS=\"\\0\"} NF{split($0,a,\"\\n\"); printf \"%s\\0\", a[1]}' {+f}");
    const QString queueAction = QStringLiteral("execute-silent(%1 | %2 enqueue --stdin0)+clear-selection")
                                    .arg(extract, self);
    const QString playAction = QStringLiteral("execute-silent(%1 | %2 enqueue --stdin0 --play)+clear-selection")
                                   .arg(extract, self);

    QStringList args{
        QStringLiteral("--read0"), QStringLiteral("--ansi"), QStringLiteral("--multi"),
        QStringLiteral("--height=100%"), QStringLiteral("--cycle"), QStringLiteral("--gap=1"),
        QStringLiteral("--highlight-line"), QStringLiteral("--delimiter=\n"),
        QStringLiteral("--with-nth=2.."),
        QStringLiteral("--prompt=♪ "), QStringLiteral("--marker-multi-line=╻┃╹"),
        QStringLiteral("--preview=printf '%s\\n' {} | tail -n +2"),
        QStringLiteral("--preview-window=down:4:wrap:hidden"),
        QStringLiteral("--bind=return:") + queueAction,
        QStringLiteral("--bind=alt-return:") + playAction,
        QStringLiteral("--bind=tab:toggle+down"),
        QStringLiteral("--bind=ctrl-space:toggle"),
        QStringLiteral("--bind=ctrl-a:select-all"),
        QStringLiteral("--bind=ctrl-/:toggle-preview"),
        QStringLiteral("--bind=esc:cancel"),
    };
    if (!fuzzy) {
        args.prepend(QStringLiteral("--exact"));
    }

    QProcess fzf;
    fzf.setProgram(fzfPath);
    fzf.setArguments(args);
    fzf.setProcessChannelMode(QProcess::ForwardedChannels); // fzf draws on the controlling tty
    fzf.start();
    if (!fzf.waitForStarted(3000)) {
        return fail(QStringLiteral("could not start fzf"));
    }
    // fzf is now visible (empty). Stream records into it as they load so rows
    // appear progressively instead of after a blocking full read.
    const auto sink = [&fzf](const Search::SearchRecord &r) {
        if (fzf.state() == QProcess::NotRunning) {
            return; // user already quit; stop feeding
        }
        fzf.write(pickerRecord(r));
        if (fzf.bytesToWrite() > (1 << 20)) {
            fzf.waitForBytesWritten(200); // bounded: don't stall if fzf exited
        }
    };
    const SearchCli::LoadResult load = SearchCli::streamRecords(sink, refresh);
    if (!load.ok) {
        fzf.closeWriteChannel();
        fzf.terminate();
        fzf.waitForFinished(2000);
        return fail(load.error);
    }
    fzf.closeWriteChannel();
    fzf.waitForFinished(-1);
    return 0;
}

// `muzaitenctl search` — standalone, fold-aware library search. Opens the
// library DB directly and queries the folded-index cache (no running instance
// required). With no query in a terminal it launches an fzf picker; piped, it
// dumps the whole library. Default output is TSV; --plain a human block; --json
// an array.
int runSearch(QStringList arguments, bool json)
{
    bool refresh = false, plain = false, fuzzy = false, clearCache = false;
    int limit = 0;
    QStringList queryWords;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString w = arguments.at(i);
        if (w == QLatin1String("--limit")) {
            bool ok = false;
            if (i + 1 >= arguments.size() || (limit = arguments.at(++i).toInt(&ok)) <= 0 || !ok) {
                return fail(QStringLiteral("search --limit needs a positive number"));
            }
        } else if (w == QLatin1String("--refresh")) {
            refresh = true;
        } else if (w == QLatin1String("--plain")) {
            plain = true;
        } else if (w == QLatin1String("--fuzzy")) {
            fuzzy = true;
        } else if (w == QLatin1String("--clear-cache")) {
            clearCache = true;
        } else if (w.startsWith(QLatin1String("--"))) {
            return fail(QStringLiteral("unknown search option \"%1\"").arg(w));
        } else {
            queryWords.push_back(w);
        }
    }

    if (clearCache) {
        QString path;
        const bool removed = SearchCli::clearCache(&path);
        std::fprintf(stderr, removed ? "cleared cache: %s\n" : "no cache to clear: %s\n", qPrintable(path));
        return 0;
    }

    // No query on a terminal with fzf → interactive picker. It loads (and streams)
    // its own data, so we dispatch before the blocking loadIndex below.
    if (queryWords.isEmpty()) {
        const bool isTty = ::isatty(fileno(stdout)) != 0;
        const QString fzf = QStandardPaths::findExecutable(QStringLiteral("fzf"));
        if (isTty && !fzf.isEmpty() && !plain && !json) {
            return runPicker(fzf, fuzzy, refresh);
        }
        if (isTty && fzf.isEmpty()) {
            return fail(QStringLiteral("no query given and fzf not found; install fzf for the "
                                       "interactive picker, pass a query, or pipe for a full dump"));
        }
        // Piped / --plain / --json with no query → fall through to the full dump.
    }

    Search::SearchIndex index;
    const SearchCli::LoadResult load = SearchCli::loadIndex(index, refresh);
    if (!load.ok) {
        return fail(load.error);
    }
    if (load.wasStale) {
        std::fprintf(stderr, "muzaitenctl: note: search cache is stale; "
                             "run 'muzaitenctl search --refresh' to update\n");
    }

    // Select which records to emit: the matched set, or the whole library (dump).
    QVector<Search::ScoredResult> results; // owns the matched records; outlives the pointers below
    QVector<const Search::SearchRecord *> records;
    if (queryWords.isEmpty()) {
        for (const Search::SearchRecord &r : index.records()) {
            records.push_back(&r);
        }
    } else {
        const Search::SearchQuery query = Search::SearchQuery::parse(queryWords.join(QLatin1Char(' ')));
        int total = 0;
        results = index.match(query, fuzzy, {}, &total);
        records.reserve(results.size());
        for (const Search::ScoredResult &sr : results) {
            records.push_back(&sr.rec);
        }
    }
    if (limit > 0 && records.size() > limit) {
        records.resize(limit);
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    if (json) {
        QJsonArray array;
        for (const Search::SearchRecord *r : records) {
            array.append(QJsonObject{
                {QStringLiteral("path"), r->path},
                {QStringLiteral("title"), r->title},
                {QStringLiteral("artist"), r->artistName},
                {QStringLiteral("album"), r->albumTitle},
                {QStringLiteral("date"), r->date},
                {QStringLiteral("duration"), std::round(static_cast<double>(r->durationMs) / 10.0) / 100.0},
                {QStringLiteral("rating"), r->rating0To100},
            });
        }
        out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << '\n';
        return 0;
    }

    for (const Search::SearchRecord *r : records) {
        if (plain) {
            out << searchHumanLine(*r) << '\n' << "  " << r->path << '\n';
        } else {
            const QString rating = r->rating0To100 >= 0 ? QString::number(r->rating0To100) : QString();
            out << tsvField(r->path) << '\t' << tsvField(r->title) << '\t' << tsvField(r->artistName) << '\t'
                << tsvField(r->albumTitle) << '\t' << tsvField(r->date) << '\t'
                << r->durationMs << '\t' << rating << '\n';
        }
    }
    return 0;
}

int runSemanticSearch(QStringList arguments, bool json)
{
    bool jsonOut = json;
    bool useCache = true;
    int limit = defaultSemanticSearchLimit;
    QByteArray queryVectorJson;
    QStringList queryWords;

    for (int i = 0; i < arguments.size(); ++i) {
        const QString word = arguments.at(i);
        if (word == QLatin1String("--json")) {
            jsonOut = true;
        } else if (word == QLatin1String("--no-cache")) {
            useCache = false;
        } else if (word == QLatin1String("--limit")) {
            bool ok = false;
            if (i + 1 >= arguments.size() || (limit = arguments.at(++i).toInt(&ok)) <= 0 || !ok) {
                return fail(QStringLiteral("semantic-search --limit needs a positive number"));
            }
        } else if (word == QLatin1String("--query-vector-json")) {
            if (i + 1 >= arguments.size()) {
                return fail(QStringLiteral("semantic-search --query-vector-json needs a JSON vector"));
            }
            queryVectorJson = arguments.at(++i).toUtf8();
        } else if (word.startsWith(QLatin1String("--"))) {
            return fail(QStringLiteral("unknown semantic-search option \"%1\"").arg(word));
        } else {
            queryWords.push_back(word);
        }
    }

    if (queryWords.isEmpty()) {
        return fail(QStringLiteral("semantic-search needs a text query"));
    }
    const QString queryText = queryWords.join(QLatin1Char(' ')).trimmed();
    if (queryText.isEmpty()) {
        return fail(QStringLiteral("semantic-search needs a non-empty text query"));
    }

    const QString featurePath = featuresDbPath();
    if (!QFileInfo::exists(featurePath)) {
        return fail(QStringLiteral("features.sqlite not found at %1").arg(featurePath));
    }
    FeatureStore features(featurePath);
    if (!features.isOpen()) {
        return fail(QStringLiteral("features.sqlite unsupported or unreadable at %1").arg(featurePath));
    }
    if (!QFileInfo::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }
    Database db(QStringLiteral("muzaitenctl-semantic-search-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }

    // The cache key is the active generation's model identity: a hit is by
    // construction provenance-correct, so it skips both the provider process
    // and the metadata comparison below.
    QueryVectorCache::Identity cacheIdentity;
    if (features.schemaVersion() >= 5) {
        const auto generation = features.activeSemanticGeneration();
        if (generation.valid()) {
            cacheIdentity = {generation.capability, generation.model, generation.checkpointSha256,
                             generation.featureRevision, generation.vectorDimension};
        }
    }

    QString vectorError;
    QJsonObject queryMetadata;
    QVector<float> queryVector;
    bool fromCache = false;
    if (!queryVectorJson.isEmpty()) {
        queryVector = QueryEmbedding::parseVectorJson(queryVectorJson, &vectorError);
    } else if (useCache && cacheIdentity.valid()) {
        QueryVectorCache cache(QueryVectorCache::defaultPath());
        queryVector = cache.lookup(cacheIdentity, queryText);
        fromCache = !queryVector.isEmpty();
    }
    if (queryVector.isEmpty() && queryVectorJson.isEmpty()) {
        QueryEmbedding::Result embedded = QueryEmbedding::viaFeatures(queryText, semanticQueryTimeoutMs);
        queryVector = embedded.vector;
        queryMetadata = embedded.metadata;
        vectorError = embedded.error;
    }
    if (queryVector.isEmpty()) {
        return fail(vectorError.isEmpty() ? QStringLiteral("semantic-search could not build a query embedding")
                                          : vectorError);
    }
    if (queryVectorJson.isEmpty() && !fromCache && features.schemaVersion() >= 5) {
        const auto generation = features.activeSemanticGeneration();
        const bool matches = generation.valid()
            && queryMetadata.value(QStringLiteral("capability")).toString() == generation.capability
            && queryMetadata.value(QStringLiteral("model")).toString() == generation.model
            && queryMetadata.value(QStringLiteral("checkpoint_sha256")).toString()
                == generation.checkpointSha256
            && queryMetadata.value(QStringLiteral("feature_revision")).toString()
                == generation.featureRevision
            && queryMetadata.value(QStringLiteral("dim")).toInt() == generation.vectorDimension;
        if (!matches) {
            return fail(QStringLiteral(
                "semantic query generation does not match the active features.sqlite generation; refresh semantic features"));
        }
        if (useCache && cacheIdentity.valid()) {
            QueryVectorCache cache(QueryVectorCache::defaultPath());
            cache.store(cacheIdentity, queryText, queryVector);
        }
    }

    QString rankError;
    const QVector<SemanticSearchResult> results = rankSemanticMatches(queryVector, features, db, limit, &rankError);
    if (results.isEmpty() && !rankError.isEmpty()) {
        return fail(rankError);
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    if (jsonOut) {
        QJsonArray array;
        for (const SemanticSearchResult &result : results) {
            array.append(semanticResultJson(result));
        }
        out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << '\n';
        return 0;
    }

    for (const SemanticSearchResult &result : results) {
        const Track &track = result.member.track;
        out << QString::number(result.score, 'f', 6) << '\t'
            << tsvField(track.path) << '\t'
            << tsvField(track.title) << '\t'
            << tsvField(track.artistName) << '\t'
            << tsvField(track.albumTitle) << '\n';
    }
    return 0;
}

int runGenreReport(QStringList arguments, bool json)
{
    bool plain = false;
    for (const QString &word : arguments) {
        if (word == QLatin1String("--plain")) {
            plain = true;
        } else if (word.startsWith(QLatin1String("--"))) {
            return fail(QStringLiteral("unknown genre-report option \"%1\"").arg(word));
        } else {
            return fail(QStringLiteral("genre-report does not take positional arguments"));
        }
    }

    if (!QFile::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }

    Database db(QStringLiteral("muzaitenctl-genre-report-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }

    int taggedTrackTotal = 0;
    const QVector<GenreCuration::ReportRow> rows = GenreCuration::buildReportRows(db, &taggedTrackTotal);
    if (json) {
        QTextStream out(stdout);
        out.setEncoding(QStringConverter::Utf8);
        out << QString::fromUtf8(QJsonDocument(genreRowsToJson(rows)).toJson(QJsonDocument::Compact)) << '\n';
    } else if (plain) {
        printGenreReportTsv(rows, taggedTrackTotal);
    } else {
        printGenreReportTable(rows, taggedTrackTotal);
    }
    return 0;
}

int runFeaturesStatus(QStringList arguments, bool json)
{
    if (!arguments.isEmpty()) {
        return fail(QStringLiteral("features-status does not take arguments"));
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    const QString path = featuresDbPath();
    if (!QFileInfo::exists(path)) {
        const QString message = QStringLiteral("features.sqlite not found");
        if (json) {
            out << QString::fromUtf8(QJsonDocument(
                featureStatusJson(path, false, false, {}, -1, message)).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            out << message << " at " << path << '\n';
        }
        return 0;
    }

    FeatureStore store(path);
    if (!store.isOpen()) {
        const QString message = QStringLiteral("features.sqlite unsupported or unreadable");
        if (json) {
            out << QString::fromUtf8(QJsonDocument(
                featureStatusJson(path, true, false, {}, -1, message)).toJson(QJsonDocument::Compact)) << '\n';
        }
        return fail(QStringLiteral("%1 at %2").arg(message, path));
    }

    const FeatureStore::Status status = store.status();
    if (json) {
        out << QString::fromUtf8(QJsonDocument(
            featureStatusJson(path, true, true, status, store.schemaVersion())).toJson(QJsonDocument::Compact)) << '\n';
        return 0;
    }

    out << "features.sqlite: " << path << '\n';
    out << "schema: " << store.schemaVersion() << '\n';
    out << "files: " << status.files << " (" << status.ok << " ok, "
        << status.failed << " failed)\n";
    out << "groups: " << status.groups << '\n';
    out << "featured: " << status.featured;
    if (status.featuredStale > 0) {
        out << " (" << status.featuredStale << " stale, awaiting re-analysis)";
    }
    out << '\n';
    out << "dsp version: " << (status.dspVersion.isEmpty() ? QStringLiteral("unknown") : status.dspVersion);
    if (!status.expectedDspVersion.isEmpty() && status.dspVersion != status.expectedDspVersion) {
        out << " (this build expects " << status.expectedDspVersion << ')';
    }
    out << '\n';
    out << "embedded groups: " << status.embeddedGroups;
    if (!status.embeddingModel.isEmpty() || !status.embeddingVersion.isEmpty()) {
        out << " (" << (status.embeddingModel.isEmpty() ? QStringLiteral("unknown-model") : status.embeddingModel)
            << ' ' << (status.embeddingVersion.isEmpty() ? QStringLiteral("unknown-version") : status.embeddingVersion)
            << ')';
    }
    out << '\n';
    out << "neighbor rows: " << status.neighborRows << '\n';
    return 0;
}

int runDuplicateGroups(QStringList arguments, bool json)
{
    int minSize = 2;
    while (!arguments.isEmpty()) {
        const QString word = arguments.takeFirst();
        if (word == QLatin1String("--min-size")) {
            if (arguments.isEmpty()) {
                return fail(QStringLiteral("duplicate-groups --min-size needs a value"));
            }
            bool ok = false;
            minSize = arguments.takeFirst().toInt(&ok);
            if (!ok || minSize < 1) {
                return fail(QStringLiteral("duplicate-groups --min-size needs a positive integer"));
            }
        } else {
            return fail(QStringLiteral("unknown duplicate-groups option \"%1\"").arg(word));
        }
    }

    const QString featurePath = featuresDbPath();
    if (!QFileInfo::exists(featurePath)) {
        return fail(QStringLiteral("features.sqlite not found at %1").arg(featurePath));
    }
    FeatureStore features(featurePath);
    if (!features.isOpen()) {
        return fail(QStringLiteral("features.sqlite unsupported or unreadable at %1").arg(featurePath));
    }
    if (!QFileInfo::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }
    Database db(QStringLiteral("muzaitenctl-duplicate-groups-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    const QHash<qint64, QString> pins = db.contentGroupPins();
    QJsonArray groupsJson;
    int printed = 0;
    for (qint64 groupId : features.contentGroupIds(minSize)) {
        const QString pinnedPath = pins.value(groupId);
        const QVector<DuplicateMember> members = duplicateMembers(db, features, groupId, pinnedPath);
        if (members.size() < minSize) {
            continue;
        }
        QVector<QualityRank::Copy> copies;
        copies.reserve(members.size());
        for (const DuplicateMember &member : members) {
            copies.push_back(qualityCopyForTrack(member.track, member.mediaTags));
        }
        const QString bestPath = QualityRank::bestPath(copies, pinnedPath);
        if (json) {
            QJsonArray memberArray;
            for (const DuplicateMember &member : members) {
                memberArray.append(duplicateMemberJson(member));
            }
            groupsJson.append(QJsonObject{
                {QStringLiteral("content_group_id"), static_cast<double>(groupId)},
                {QStringLiteral("pin"), pinnedPath},
                {QStringLiteral("best_path"), bestPath},
                {QStringLiteral("members"), memberArray},
            });
        } else {
            out << "group " << groupId << " (" << members.size() << " copies";
            if (!bestPath.isEmpty()) {
                out << ", best " << bestPath;
            }
            if (!pinnedPath.isEmpty()) {
                out << ", pinned " << pinnedPath;
            }
            out << ")\n";
            for (const DuplicateMember &member : members) {
                out << "  " << (member.pinned ? '*' : ' ') << ' '
                    << qualitySummary(member) << '\t' << member.track.path << '\n';
            }
        }
        ++printed;
    }

    if (json) {
        out << QString::fromUtf8(QJsonDocument(groupsJson).toJson(QJsonDocument::Compact)) << '\n';
    } else if (printed == 0) {
        out << "duplicate-groups: no groups with at least " << minSize << " library members\n";
    }
    return 0;
}

int runPinCopy(QStringList arguments, bool json)
{
    if (arguments.size() != 2) {
        return fail(QStringLiteral("pin-copy needs <group-id> <path>"));
    }
    bool ok = false;
    const qint64 groupId = arguments.at(0).toLongLong(&ok);
    if (!ok || groupId < 0) {
        return fail(QStringLiteral("pin-copy needs a non-negative group id"));
    }
    const QString path = arguments.at(1);
    const QString featurePath = featuresDbPath();
    if (!QFileInfo::exists(featurePath)) {
        return fail(QStringLiteral("features.sqlite not found at %1").arg(featurePath));
    }
    FeatureStore features(featurePath);
    if (!features.isOpen()) {
        return fail(QStringLiteral("features.sqlite unsupported or unreadable at %1").arg(featurePath));
    }
    if (!features.pathsInGroup(groupId).contains(path)) {
        return fail(QStringLiteral("path is not a member of content group %1").arg(groupId));
    }
    if (!QFileInfo::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }
    Database db(QStringLiteral("muzaitenctl-pin-copy-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }
    if (!db.setContentGroupPin(groupId, path)) {
        return fail(db.lastError());
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    if (json) {
        out << QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("content_group_id"), static_cast<double>(groupId)},
            {QStringLiteral("pinned_path"), path},
        }).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        out << "pin-copy: group " << groupId << " -> " << path << '\n';
    }
    return 0;
}

int runUnpinCopy(QStringList arguments, bool json)
{
    if (arguments.size() != 1) {
        return fail(QStringLiteral("unpin-copy needs <group-id>"));
    }
    bool ok = false;
    const qint64 groupId = arguments.at(0).toLongLong(&ok);
    if (!ok || groupId < 0) {
        return fail(QStringLiteral("unpin-copy needs a non-negative group id"));
    }
    if (!QFileInfo::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }
    Database db(QStringLiteral("muzaitenctl-unpin-copy-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }
    if (!db.removeContentGroupPin(groupId)) {
        return fail(db.lastError());
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    if (json) {
        out << QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("content_group_id"), static_cast<double>(groupId)},
        }).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        out << "unpin-copy: removed pin for group " << groupId << '\n';
    }
    return 0;
}

int runRadioGenre(QStringList arguments, bool json)
{
    if (arguments.isEmpty()) {
        return fail(QStringLiteral("radio-genre needs a verb: ignore, unignore, or list"));
    }
    const QString verb = arguments.takeFirst();
    if (verb != QLatin1String("ignore") && verb != QLatin1String("unignore") && verb != QLatin1String("list")) {
        return fail(QStringLiteral("radio-genre verb must be ignore, unignore, or list"));
    }
    if (verb == QLatin1String("list") && !arguments.isEmpty()) {
        return fail(QStringLiteral("radio-genre list does not take arguments"));
    }
    if (verb != QLatin1String("list") && arguments.size() != 1) {
        return fail(QStringLiteral("radio-genre %1 needs exactly one genre").arg(verb));
    }
    if (!QFile::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }

    Database db(QStringLiteral("muzaitenctl-radio-genre-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    if (verb == QLatin1String("list")) {
        QStringList ignored;
        for (const QString &genre : db.ignoredRadioGenres()) {
            ignored.push_back(genre);
        }
        ignored.sort(Qt::CaseInsensitive);
        if (json) {
            QJsonArray array;
            for (const QString &genre : ignored) {
                array.append(genre);
            }
            out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            for (const QString &genre : ignored) {
                out << genre << '\n';
            }
        }
        return 0;
    }

    QString genreError;
    const QString canonical = GenreCuration::canonicalGenreInput(db, arguments.first(), &genreError);
    if (!genreError.isEmpty()) {
        return fail(QStringLiteral("radio-genre %1 needs a non-empty genre").arg(verb));
    }
    const bool ignored = verb == QLatin1String("ignore");
    if (!db.setRadioGenreIgnored(canonical, ignored)) {
        return fail(db.lastError());
    }
    if (json) {
        out << QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("action"), ignored ? QStringLiteral("ignore") : QStringLiteral("unignore")},
            {QStringLiteral("genre"), canonical},
        }).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        out << "radio-genre: " << (ignored ? QStringLiteral("ignored ") : QStringLiteral("unignored "))
            << canonical << '\n';
    }
    return 0;
}

int runGenreAlias(QStringList arguments, bool json)
{
    if (arguments.isEmpty()) {
        return fail(QStringLiteral("genre-alias needs a verb: set, remove, or list"));
    }
    const QString verb = arguments.takeFirst();
    if (verb != QLatin1String("set") && verb != QLatin1String("remove") && verb != QLatin1String("list")) {
        return fail(QStringLiteral("genre-alias verb must be set, remove, or list"));
    }
    if (verb == QLatin1String("list") && !arguments.isEmpty()) {
        return fail(QStringLiteral("genre-alias list does not take arguments"));
    }
    if (verb == QLatin1String("remove") && arguments.size() != 1) {
        return fail(QStringLiteral("genre-alias remove needs exactly one alias"));
    }
    if (verb == QLatin1String("set") && arguments.size() != 2) {
        return fail(QStringLiteral("genre-alias set needs exactly alias and canonical arguments"));
    }
    if (!QFile::exists(SearchCli::libraryDbPath())) {
        return fail(QStringLiteral("library database not found at %1").arg(SearchCli::libraryDbPath()));
    }

    Database db(QStringLiteral("muzaitenctl-genre-alias-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(SearchCli::libraryDbPath())) {
        return fail(db.lastError());
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    if (verb == QLatin1String("list")) {
        const QHash<QString, QString> aliases = db.genreAliases();
        QStringList keys = aliases.keys();
        keys.sort(Qt::CaseInsensitive);
        if (json) {
            QJsonArray array;
            for (const QString &alias : keys) {
                array.append(QJsonObject{
                    {QStringLiteral("alias"), alias},
                    {QStringLiteral("canonical"), aliases.value(alias)},
                });
            }
            out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            for (const QString &alias : keys) {
                out << alias << '\t' << aliases.value(alias) << '\n';
            }
        }
        return 0;
    }

    const QString alias = GenreTags::folded(arguments.first());
    if (alias.isEmpty()) {
        return fail(QStringLiteral("genre-alias %1 needs a non-empty alias").arg(verb));
    }
    if (verb == QLatin1String("remove")) {
        if (!db.removeGenreAlias(alias)) {
            return fail(db.lastError());
        }
        if (json) {
            out << QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("action"), QStringLiteral("remove")},
                {QStringLiteral("alias"), alias},
            }).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            out << "genre-alias: removed " << alias << '\n';
        }
        return 0;
    }

    const GenreCuration::AliasValidation validation = GenreCuration::validateAlias(arguments.first(), arguments.at(1));
    if (!validation.ok()) {
        return fail(validation.error);
    }
    if (!db.setGenreAlias(validation.aliasFolded, validation.canonicalFolded)) {
        return fail(db.lastError());
    }
    if (json) {
        out << QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("action"), QStringLiteral("set")},
            {QStringLiteral("alias"), validation.aliasFolded},
            {QStringLiteral("canonical"), validation.canonicalFolded},
        }).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        out << "genre-alias: " << validation.aliasFolded << " -> " << validation.canonicalFolded << '\n';
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();

    const bool json = arguments.removeAll(QStringLiteral("--json")) > 0;
    if (arguments.isEmpty() || arguments.first() == QLatin1String("--help") || arguments.first() == QLatin1String("-h")) {
        printUsage();
        return arguments.isEmpty() ? 1 : 0;
    }

    QString command = arguments.takeFirst();
    QJsonObject args;

    if (command == QLatin1String("toggle")) {
        command = QStringLiteral("play-pause");
    }
    if (command == QLatin1String("previous")) {
        command = QStringLiteral("prev");
    }

    // `search` is handled entirely client-side (opens the library DB directly),
    // so it works without a running instance and never touches the socket.
    if (command == QLatin1String("search")) {
        return runSearch(arguments, json);
    }
    if (command == QLatin1String("semantic-search")) {
        return runSemanticSearch(arguments, json);
    }
    if (command == QLatin1String("genre-report")) {
        return runGenreReport(arguments, json);
    }
    if (command == QLatin1String("features-status")) {
        return runFeaturesStatus(arguments, json);
    }
    if (command == QLatin1String("duplicate-groups")) {
        return runDuplicateGroups(arguments, json);
    }
    if (command == QLatin1String("pin-copy")) {
        return runPinCopy(arguments, json);
    }
    if (command == QLatin1String("unpin-copy")) {
        return runUnpinCopy(arguments, json);
    }
    if (command == QLatin1String("radio-genre")) {
        return runRadioGenre(arguments, json);
    }
    if (command == QLatin1String("radio-weights")) {
        return runRadioWeights(arguments, json);
    }
    if (command == QLatin1String("radio-learn")) {
        return runRadioLearn(arguments, json);
    }
    if (command == QLatin1String("genre-alias")) {
        return runGenreAlias(arguments, json);
    }

    if (command == QLatin1String("jump")
        || (command == QLatin1String("queue") && !arguments.isEmpty())) {
        bool ok = false;
        const int index = arguments.first().toInt(&ok);
        if (!ok || index < 0) {
            return fail(QStringLiteral("queue jump needs a row index"));
        }
        command = QStringLiteral("queue-jump");
        args.insert(QStringLiteral("index"), index);
    } else if (command == QLatin1String("enqueue")) {
        QJsonArray paths;
        bool play = false;
        bool next = false;
        bool stdin0 = false;
        for (const QString &word : arguments) {
            if (word == QLatin1String("--play")) {
                play = true;
            } else if (word == QLatin1String("--next")) {
                next = true;
            } else if (word == QLatin1String("--stdin0")) {
                stdin0 = true; // read NUL-separated paths from stdin (used by the picker)
            } else if (word.startsWith(QLatin1String("--"))) {
                return fail(QStringLiteral("unknown enqueue option \"%1\"").arg(word));
            } else {
                // Resolve against the client's cwd; the server has its own.
                paths.append(QFileInfo(word).absoluteFilePath());
            }
        }
        if (stdin0) {
            QFile in;
            if (in.open(stdin, QIODevice::ReadOnly)) {
                const QList<QByteArray> chunks = in.readAll().split('\0');
                for (const QByteArray &chunk : chunks) {
                    if (!chunk.isEmpty()) {
                        paths.append(QString::fromUtf8(chunk));
                    }
                }
            }
        }
        if (paths.isEmpty()) {
            return fail(QStringLiteral("enqueue needs at least one path"));
        }
        args.insert(QStringLiteral("paths"), paths);
        if (play) {
            args.insert(QStringLiteral("play"), true);
        }
        if (next) {
            args.insert(QStringLiteral("next"), true);
        }
    } else if (command == QLatin1String("scrobble-backfill")) {
        if (arguments.isEmpty()) {
            return fail(QStringLiteral("scrobble-backfill needs a service: listenbrainz, lastfm, status, or cancel"));
        }
        const QString service = arguments.first().toLower();
        if (service == QLatin1String("reset")) {
            if (arguments.size() < 2) {
                return fail(QStringLiteral("scrobble-backfill reset needs a service: listenbrainz or lastfm"));
            }
            const QString target = arguments.at(1).toLower();
            if (target != QLatin1String("listenbrainz") && target != QLatin1String("lastfm")) {
                return fail(QStringLiteral("scrobble-backfill reset service must be listenbrainz or lastfm"));
            }
            args.insert(QStringLiteral("service"), service);
            args.insert(QStringLiteral("target"), target);
        } else if (service != QLatin1String("listenbrainz") && service != QLatin1String("lastfm")
                   && service != QLatin1String("status") && service != QLatin1String("cancel")) {
            return fail(QStringLiteral("scrobble-backfill service must be listenbrainz, lastfm, status, cancel, or reset"));
        } else {
            args.insert(QStringLiteral("service"), service);
        }
    } else if (command == QLatin1String("start-radio")) {
        if (arguments.isEmpty()) {
            return fail(QStringLiteral("start-radio needs a library track path"));
        }
        // Resolve against the client's cwd; the server has its own.
        args.insert(QStringLiteral("path"), QFileInfo(arguments.first()).absoluteFilePath());
    } else if (command == QLatin1String("start-artist-radio")) {
        if (arguments.isEmpty()) {
            return fail(QStringLiteral("start-artist-radio needs an artist name"));
        }
        args.insert(QStringLiteral("artist"), arguments.join(QLatin1Char(' ')));
    } else if (command == QLatin1String("start-mix")) {
        if (arguments.isEmpty()) {
            return fail(QStringLiteral("start-mix needs a mode: rediscovery or deepcuts"));
        }
        args.insert(QStringLiteral("mode"), arguments.first().toLower());
    } else if (command == QLatin1String("radio-reasons")) {
        if (!arguments.isEmpty()) {
            return fail(QStringLiteral("radio-reasons does not take arguments"));
        }
    } else if (command == QLatin1String("play-file")) {
        if (arguments.isEmpty()) {
            return fail(QStringLiteral("play-file needs a path"));
        }
        // Resolve against the client's cwd; the server has its own.
        args.insert(QStringLiteral("path"), QFileInfo(arguments.first()).absoluteFilePath());
    } else if (command == QLatin1String("seek")) {
        double seconds = 0.0;
        bool relative = false;
        if (arguments.isEmpty() || !parseTime(arguments.first(), seconds, relative)) {
            return fail(QStringLiteral("seek needs a position: seconds, mm:ss, or +/-seconds"));
        }
        args.insert(relative ? QStringLiteral("offset_ms") : QStringLiteral("ms"), seconds * 1000.0);
    } else if (command == QLatin1String("volume")) {
        if (arguments.isEmpty()) {
            return fail(QStringLiteral("volume needs a value: 0-100 or +/-N"));
        }
        const QString value = arguments.first();
        bool ok = false;
        const double percent = value.toDouble(&ok);
        if (!ok) {
            return fail(QStringLiteral("volume value \"%1\" is not a number").arg(value));
        }
        const bool relative = value.startsWith(QLatin1Char('+')) || value.startsWith(QLatin1Char('-'));
        args.insert(relative ? QStringLiteral("delta_percent") : QStringLiteral("percent"), percent);
    } else if (command == QLatin1String("rate")) {
        if (arguments.isEmpty()) {
            return fail(QStringLiteral("rate needs a value: 0-5 stars, \"raw <0-100>\", or \"clear\""));
        }
        QString value = arguments.takeFirst();
        if (value == QLatin1String("clear")) {
            args.insert(QStringLiteral("clear"), true);
        } else {
            bool raw = false;
            if (value == QLatin1String("raw")) {
                raw = true;
                if (arguments.isEmpty()) {
                    return fail(QStringLiteral("rate raw needs a 0-100 value"));
                }
                value = arguments.takeFirst();
            }
            bool ok = false;
            const double number = value.toDouble(&ok);
            const double max = raw ? 100.0 : 5.0;
            if (!ok || number < 0.0 || number > max) {
                return fail(QStringLiteral("rate value \"%1\" must be 0-%2").arg(value).arg(max));
            }
            args.insert(QStringLiteral("rating"),
                        static_cast<int>(std::lround(raw ? number : number * 20.0)));
        }
    }

    QLocalSocket socket;
    socket.connectToServer(IpcSocket::serverPath());
    if (!socket.waitForConnected(connectTimeoutMs)) {
        return fail(QStringLiteral("cannot connect to %1 (is muzaiten running?)").arg(IpcSocket::serverPath()));
    }

    const QJsonObject request{{QStringLiteral("command"), command}, {QStringLiteral("args"), args}};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');

    QByteArray reply;
    while (!reply.contains('\n')) {
        if (!socket.waitForReadyRead(replyTimeoutMs)) {
            return fail(QStringLiteral("no reply from muzaiten"));
        }
        reply += socket.readAll();
    }

    const QJsonObject response = QJsonDocument::fromJson(reply.left(reply.indexOf('\n'))).object();
    if (json) {
        std::printf("%s\n", QJsonDocument(response).toJson(QJsonDocument::Compact).constData());
        return response.value(QStringLiteral("ok")).toBool() ? 0 : 1;
    }
    if (!response.value(QStringLiteral("ok")).toBool()) {
        return fail(response.value(QStringLiteral("error")).toString(QStringLiteral("unknown error")));
    }
    if (command == QLatin1String("queue")) {
        const QJsonArray tracks = response.value(QStringLiteral("tracks")).toArray();
        const int current = response.value(QStringLiteral("index")).toInt(-1);
        for (qsizetype i = 0; i < tracks.size(); ++i) {
            std::printf("%c %3lld  %s\n", i == current ? '>' : ' ', static_cast<long long>(i),
                        qPrintable(trackLine(tracks.at(i).toObject())));
        }
        if (tracks.isEmpty()) {
            std::printf("queue is empty\n");
        }
        return 0;
    }
    if (command == QLatin1String("scrobble-backfill")) {
        // "status" replies carry BackfillStatus fields directly (no "backfill"
        // key); "cancel" and the service starters reply with {"backfill": ...}.
        if (!response.contains(QStringLiteral("backfill"))) {
            const QString service = response.value(QStringLiteral("service")).toString();
            const bool running = response.value(QStringLiteral("running")).toBool();
            std::printf("service: %s\n", qPrintable(service.isEmpty() ? QStringLiteral("(none)") : service));
            std::printf("running: %s\n", running ? "yes" : "no");
            std::printf("processed: %s\n", qPrintable(QString::number(response.value(QStringLiteral("processed")).toDouble(), 'f', 0)));
            std::printf("stored: %s\n", qPrintable(QString::number(response.value(QStringLiteral("inserted")).toDouble(), 'f', 0)));
            const qint64 reachedTs = static_cast<qint64>(response.value(QStringLiteral("reachedTs")).toDouble());
            if (reachedTs > 0) {
                std::printf("reached: %s\n", qPrintable(QDateTime::fromSecsSinceEpoch(reachedTs).toString(Qt::ISODate)));
            }
            const qint64 total = static_cast<qint64>(response.value(QStringLiteral("totalListens")).toDouble());
            if (total > 0) {
                std::printf("total: %s\n", qPrintable(QString::number(total)));
            }
            const QString message = response.value(QStringLiteral("message")).toString();
            if (!message.isEmpty()) {
                std::printf("message: %s\n", qPrintable(message));
            }
            return 0;
        }
        const QString service = response.value(QStringLiteral("service")).toString();
        const QString state = response.value(QStringLiteral("backfill")).toString(QStringLiteral("started"));
        if (service.isEmpty()) {
            std::printf("%s\n", qPrintable(state));
        } else {
            std::printf("%s: %s\n", qPrintable(service), qPrintable(state));
        }
        return 0;
    }
    if (command == QLatin1String("start-radio") || command == QLatin1String("start-artist-radio")
        || command == QLatin1String("stop-radio")) {
        std::printf("radio: %s\n", qPrintable(response.value(QStringLiteral("radio")).toString()));
        return 0;
    }
    if (command == QLatin1String("start-mix")) {
        std::printf("mix: %s\n", qPrintable(response.value(QStringLiteral("mix")).toString()));
        return 0;
    }
    if (command == QLatin1String("radio-reasons")) {
        printRadioReasons(response);
        return 0;
    }
    // Plain "status" replies carry the canonical track JSON at the top level,
    // while command replies wrap it under response["status"].
    QJsonObject statusObject = response;
    const QJsonObject nestedStatus = response.value(QStringLiteral("status")).toObject();
    if (nestedStatus.value(QStringLiteral("playback")).isObject()
        || nestedStatus.value(QStringLiteral("track")).isObject()) {
        statusObject = nestedStatus;
    }
    printStatus(statusObject);
    return 0;
}
