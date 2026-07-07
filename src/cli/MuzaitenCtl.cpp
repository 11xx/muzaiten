// muzaitenctl — command-line client for the muzaiten IPC socket.
//
// Talks newline-delimited JSON to the IpcServer inside a running muzaiten
// instance (same state-root resolution rules, so MUZAITEN_* env vars select
// which instance). Designed for scripting and compositor keybinds:
//   muzaitenctl rate 4 && notify-send "rated $(muzaitenctl status --format artist-title)"

#include "cli/SearchCli.h"
#include "core/GenreTags.h"
#include "db/Database.h"
#include "ipc/IpcSocket.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <unistd.h>

namespace {

constexpr int connectTimeoutMs = 2000;
constexpr int replyTimeoutMs = 5000;

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
        "  genre-report [--plain]  dump folded genre vocabulary stats (works offline)\n"
        "  radio-genre ignore <genre> | unignore <genre> | list\n"
        "                          curate radio-only ignored folded genres (works offline)\n"
        "  genre-alias set <alias> <canonical> | remove <alias> | list\n"
        "                          curate folded genre aliases (works offline)\n"
        "  play-file <path>        append a file to the queue and play it\n"
        "  enqueue [--play|--next] <path...>  add files to the queue\n"
        "  raise                   show and focus the running instance's window\n"
        "  scrobble-backfill <listenbrainz|lastfm>  import listening history / sync play counts\n"
        "  scrobble-backfill status                 show progress of the current/last backfill\n"
        "  scrobble-backfill cancel                 cancel a running backfill (stops auto-resume)\n"
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

bool hasNonAsciiLetter(const QString &s)
{
    for (const QChar c : s) {
        if (c.isLetter() && c.unicode() > 127) {
            return true;
        }
    }
    return false;
}

QString punctuationFoldKey(QString s)
{
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]"));
    s.remove(nonAlnum);
    return s;
}

bool looksLikeClassifierToken(const QString &genre)
{
    static const QRegularExpression classifier(QStringLiteral(
        "^(?:[a-z]{1,4}:[^\\s].*|\\d+_information:.*|[a-z0-9_.-]{1,32}:[^\\s].*)$"));
    return classifier.match(genre).hasMatch();
}

struct GenreReportRow {
    QString genre;
    int df = 0;
    double idf = 0.0;
    QString canonical;
    QString status;
    QStringList sampleArtists;
    QStringList flags;
};

QVector<GenreReportRow> buildGenreReportRows(Database &db, int *taggedTrackTotal)
{
    int total = 0;
    const QHash<QString, int> counts = db.genreTrackCounts(&total);
    if (taggedTrackTotal != nullptr) {
        *taggedTrackTotal = total;
    }

    const QHash<QString, QString> aliases = db.genreAliases();
    const QSet<QString> ignored = db.ignoredRadioGenres();
    QHash<QString, int> canonicalDf;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        const QString canonical = GenreTags::canonical(it.key(), aliases);
        if (canonical.isEmpty() || GenreTags::isNonGenre(canonical)) {
            continue;
        }
        canonicalDf[canonical] += it.value();
    }

    QHash<QString, QStringList> nearDupGroups;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        const QString key = punctuationFoldKey(it.key());
        if (!key.isEmpty()) {
            nearDupGroups[key].append(it.key());
        }
    }
    for (QStringList &group : nearDupGroups) {
        group.sort(Qt::CaseInsensitive);
    }

    QVector<GenreReportRow> rows;
    rows.reserve(counts.size());
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        GenreReportRow row;
        row.genre = it.key();
        row.df = it.value();
        row.canonical = GenreTags::canonical(row.genre, aliases);
        const bool stoplisted = GenreTags::isNonGenre(row.canonical);
        if (ignored.contains(row.canonical)) {
            row.status = QStringLiteral("ignored");
        } else if (stoplisted) {
            row.status = QStringLiteral("stoplist");
        } else if (row.canonical != row.genre) {
            row.status = QStringLiteral("alias");
        } else {
            row.status = QStringLiteral("ok");
        }

        const int dfForIdf = stoplisted ? row.df : canonicalDf.value(row.canonical, row.df);
        row.idf = std::log(std::max(2.0, static_cast<double>(total) / std::max(1, dfForIdf)));
        row.sampleArtists = db.sampleArtistsForGenre(row.genre, 3);

        const QString key = punctuationFoldKey(row.genre);
        const QStringList nearDups = nearDupGroups.value(key);
        if (nearDups.size() > 1) {
            for (const QString &other : nearDups) {
                if (other != row.genre) {
                    row.flags.append(QStringLiteral("neardup:%1").arg(other));
                    break;
                }
            }
        }
        if (hasNonAsciiLetter(row.genre)) {
            row.flags.append(QStringLiteral("nonascii"));
        }
        if (row.genre.contains(QLatin1Char('|')) || row.genre.contains(QLatin1Char(';'))
            || row.genre.contains(QLatin1Char('/'))) {
            row.flags.append(QStringLiteral("separator"));
        }
        if (looksLikeClassifierToken(row.genre)) {
            row.flags.append(QStringLiteral("classifier"));
        }

        rows.append(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const GenreReportRow &a, const GenreReportRow &b) {
        if (a.df != b.df) {
            return a.df > b.df;
        }
        return QString::compare(a.genre, b.genre, Qt::CaseInsensitive) < 0;
    });
    return rows;
}

QJsonArray genreRowsToJson(const QVector<GenreReportRow> &rows)
{
    QJsonArray array;
    for (const GenreReportRow &row : rows) {
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

void printGenreReportTable(const QVector<GenreReportRow> &rows, int taggedTrackTotal)
{
    int genreWidth = 5;
    int canonicalWidth = 9;
    int sampleWidth = 14;
    for (const GenreReportRow &row : rows) {
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
    for (const GenreReportRow &row : rows) {
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

void printGenreReportTsv(const QVector<GenreReportRow> &rows, int taggedTrackTotal)
{
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << "# vocabulary_size\t" << rows.size() << "\ttagged_track_total\t" << taggedTrackTotal << '\n';
    out << "genre\tdf\tidf\tcanonical\tstatus\tsample_artists\tflags\n";
    for (const GenreReportRow &row : rows) {
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
    const QVector<GenreReportRow> rows = buildGenreReportRows(db, &taggedTrackTotal);
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

    const QString folded = GenreTags::folded(arguments.first());
    const QString canonical = GenreTags::canonical(folded, db.genreAliases());
    if (canonical.isEmpty()) {
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

    const QString canonical = GenreTags::folded(arguments.at(1));
    if (canonical.isEmpty()) {
        return fail(QStringLiteral("genre-alias set needs a non-empty canonical genre"));
    }
    if (!db.setGenreAlias(alias, canonical)) {
        return fail(db.lastError());
    }
    if (json) {
        out << QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("action"), QStringLiteral("set")},
            {QStringLiteral("alias"), alias},
            {QStringLiteral("canonical"), canonical},
        }).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        out << "genre-alias: " << alias << " -> " << canonical << '\n';
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
    if (command == QLatin1String("genre-report")) {
        return runGenreReport(arguments, json);
    }
    if (command == QLatin1String("radio-genre")) {
        return runRadioGenre(arguments, json);
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
        if (service != QLatin1String("listenbrainz") && service != QLatin1String("lastfm")
            && service != QLatin1String("status") && service != QLatin1String("cancel")) {
            return fail(QStringLiteral("scrobble-backfill service must be listenbrainz, lastfm, status, or cancel"));
        }
        args.insert(QStringLiteral("service"), service);
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
        return fail(QStringLiteral("cannot connect to %1 — is muzaiten running?").arg(IpcSocket::serverPath()));
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
