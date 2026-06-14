// muzaitenctl — command-line client for the muzaiten IPC socket.
//
// Talks newline-delimited JSON to the IpcServer inside a running muzaiten
// instance (same state-root resolution rules, so MUZAITEN_* env vars select
// which instance). Designed for scripting and compositor keybinds:
//   muzaitenctl rate 4 && notify-send "rated $(muzaitenctl status --format artist-title)"

#include "cli/SearchCli.h"
#include "ipc/IpcSocket.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStringList>
#include <QTextStream>

#include <cmath>
#include <cstdio>

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
        "  search [opts] <text>    fold-aware library search (TSV; works offline)\n"
        "      --plain               human-readable block instead of TSV\n"
        "      --limit N             cap the number of results\n"
        "      --fuzzy               fuzzy match instead of exact substring\n"
        "      --refresh             rebuild the on-disk cache from the library\n"
        "      --clear-cache         delete the cache and exit\n"
        "  play-file <path>        append a file to the queue and play it\n"
        "  enqueue [--play|--next] <path...>  add files to the queue\n"
        "  raise                   show and focus the running instance's window\n"
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
    // The track JSON nests the player state under "status"; tags/library/audio
    // sit at the top level next to it.
    const QJsonObject player = status.value(QStringLiteral("status")).toObject();
    const QJsonObject tags = status.value(QStringLiteral("tags")).toObject();
    const QJsonObject library = status.value(QStringLiteral("library")).toObject();
    const QString title = tags.value(QStringLiteral("title")).toString();
    const QString artist = tags.value(QStringLiteral("artist")).toString();
    const QString album = tags.value(QStringLiteral("album")).toString();

    if (title.isEmpty() && artist.isEmpty()) {
        std::printf("%s\n", qPrintable(player.value(QStringLiteral("playback")).toString(QStringLiteral("Stopped"))));
        return;
    }
    std::printf("%s: %s - %s%s\n",
                qPrintable(player.value(QStringLiteral("playback")).toString()),
                qPrintable(artist.isEmpty() ? QStringLiteral("?") : artist),
                qPrintable(title),
                qPrintable(album.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(album)));
    std::printf("%s/%s  vol %d%%  rating %s\n",
                qPrintable(formatSeconds(player.value(QStringLiteral("elapsed")).toDouble())),
                qPrintable(formatSeconds(player.value(QStringLiteral("duration")).toDouble())),
                static_cast<int>(player.value(QStringLiteral("volume")).toDouble()),
                qPrintable(starsText(library.value(QStringLiteral("effective_rating_0_100")).toInt(-1))));
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
    const double durationMs = track.value(QStringLiteral("durationMs")).toDouble();
    if (durationMs > 0) {
        line += QStringLiteral("  %1").arg(formatSeconds(durationMs / 1000.0));
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

// `muzaitenctl search` — standalone, fold-aware library search. Opens the
// library DB directly and queries the folded-index cache (no running instance
// required). Default output is TSV; --plain is a human block; --json an array.
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

    Search::SearchIndex index;
    const SearchCli::LoadResult load = SearchCli::loadIndex(index, refresh);
    if (!load.ok) {
        return fail(load.error);
    }
    if (load.wasStale) {
        std::fprintf(stderr, "muzaitenctl: note: search cache is stale; "
                             "run 'muzaitenctl search --refresh' to update\n");
    }

    if (queryWords.isEmpty()) {
        return fail(QStringLiteral("search needs a query"));
    }

    const Search::SearchQuery query = Search::SearchQuery::parse(queryWords.join(QLatin1Char(' ')));
    int total = 0;
    QVector<Search::ScoredResult> results = index.match(query, fuzzy, {}, &total);
    if (limit > 0 && results.size() > limit) {
        results.resize(limit);
    }

    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    if (json) {
        QJsonArray array;
        for (const Search::ScoredResult &sr : results) {
            const Search::SearchRecord &r = sr.rec;
            array.append(QJsonObject{
                {QStringLiteral("path"), r.path},
                {QStringLiteral("title"), r.title},
                {QStringLiteral("artist"), r.artistName},
                {QStringLiteral("album"), r.albumTitle},
                {QStringLiteral("date"), r.date},
                {QStringLiteral("durationMs"), static_cast<double>(r.durationMs)},
                {QStringLiteral("rating"), r.rating0To100},
            });
        }
        out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << '\n';
        return 0;
    }

    for (const Search::ScoredResult &sr : results) {
        const Search::SearchRecord &r = sr.rec;
        if (plain) {
            out << searchHumanLine(r) << '\n' << "  " << r.path << '\n';
        } else {
            const QString rating = r.rating0To100 >= 0 ? QString::number(r.rating0To100) : QString();
            out << tsvField(r.path) << '\t' << tsvField(r.title) << '\t' << tsvField(r.artistName) << '\t'
                << tsvField(r.albumTitle) << '\t' << tsvField(r.date) << '\t'
                << r.durationMs << '\t' << rating << '\n';
        }
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
        for (const QString &word : arguments) {
            if (word == QLatin1String("--play")) {
                play = true;
            } else if (word == QLatin1String("--next")) {
                next = true;
            } else if (word.startsWith(QLatin1String("--"))) {
                return fail(QStringLiteral("unknown enqueue option \"%1\"").arg(word));
            } else {
                // Resolve against the client's cwd; the server has its own.
                paths.append(QFileInfo(word).absoluteFilePath());
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
        args.insert(relative ? QStringLiteral("offsetMs") : QStringLiteral("ms"), seconds * 1000.0);
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
        args.insert(relative ? QStringLiteral("deltaPercent") : QStringLiteral("percent"), percent);
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
            args.insert(QStringLiteral("rating0To100"),
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
    // Plain "status" replies carry the player object at response["status"],
    // while command replies carry the full post-command track JSON there.
    QJsonObject statusObject = response;
    const QJsonObject nestedStatus = response.value(QStringLiteral("status")).toObject();
    if (nestedStatus.value(QStringLiteral("status")).isObject()
        || nestedStatus.value(QStringLiteral("tags")).isObject()) {
        statusObject = nestedStatus;
    }
    printStatus(statusObject);
    return 0;
}
