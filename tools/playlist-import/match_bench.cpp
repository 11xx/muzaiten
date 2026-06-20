// Offline matcher benchmark: runs PlaylistMatcher over JSONL playlists against a
// real library.sqlite and tallies/inspects the outcomes, for tuning the matcher
// against real data. Dev tool — not part of the app, not a registered test.
// Point it at a COPY of a library DB (never the live store).
//
//   playlist_match_bench <library.sqlite> <playlist.jsonl> [more.jsonl ...] [--verbose]
//
// --verbose prints one line per entry: [status] Artist - Title | resolution.

#include "db/Database.h"
#include "playlist/PlaylistImport.h"
#include "playlist/PlaylistMatcher.h"
#include "search/SearchIndex.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

const char *label(PlaylistMatcher::Decision d)
{
    using D = PlaylistMatcher::Decision;
    switch (d) {
    case D::Matched:     return "matched";
    case D::Approximate: return "approx ";
    case D::MultiMatch:  return "multi  ";
    case D::Pending:     return "pending";
    }
    return "?";
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QString libPath;
    QStringList files;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--verbose") || a == QStringLiteral("-v")) {
            verbose = true;
        } else if (libPath.isEmpty()) {
            libPath = a;
        } else {
            files << a;
        }
    }
    if (libPath.isEmpty() || files.isEmpty()) {
        std::fprintf(stderr, "usage: playlist_match_bench <library.sqlite> <playlist.jsonl ...> [--verbose]\n");
        return 2;
    }

    Database db(QStringLiteral("match-bench"));
    if (!db.open(libPath)) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(db.lastError()));
        return 1;
    }
    Search::SearchIndex index;
    index.build(db.allTracksForSearch());

    int gM = 0, gA = 0, gMu = 0, gP = 0, gTotal = 0;
    using D = PlaylistMatcher::Decision;
    for (const QString &file : files) {
        QString err;
        const auto entries = PlaylistImport::parseFile(file, &err);
        int m = 0, a = 0, mu = 0, p = 0;
        for (const auto &e : entries) {
            const auto o = PlaylistMatcher::match(index, e);
            switch (o.decision) {
            case D::Matched:     ++m;  break;
            case D::Approximate: ++a;  break;
            case D::MultiMatch:  ++mu; break;
            case D::Pending:     ++p;  break;
            }
            if (verbose) {
                const QString src = e.artist.isEmpty()
                    ? e.title : QStringLiteral("%1 - %2").arg(e.artist, e.title);
                QString resolved;
                if (o.decision == D::Matched || o.decision == D::Approximate) {
                    resolved = QStringLiteral("%1%% => %2").arg(o.confidence0To100).arg(o.best.path);
                } else if (o.decision == D::MultiMatch) {
                    resolved = QStringLiteral("%1 candidates").arg(o.candidatePaths.size());
                }
                std::printf("  [%s] %-55s | %s\n", label(o.decision),
                            qPrintable(src.left(55)), qPrintable(resolved));
            }
        }
        std::printf("%-52s n=%-5lld matched=%-4d approx=%-3d multi=%-4d pending=%-3d\n",
                    qPrintable(QFileInfo(file).fileName().left(52)),
                    static_cast<long long>(entries.size()), m, a, mu, p);
        gM += m; gA += a; gMu += mu; gP += p; gTotal += static_cast<int>(entries.size());
    }
    std::printf("---\nTOTAL n=%d  matched=%d (%.0f%%)  approx=%d  multi=%d  pending=%d\n",
                gTotal, gM, gTotal ? 100.0 * gM / gTotal : 0.0, gA, gMu, gP);
    return 0;
}
