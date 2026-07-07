#include "reco/GenreCuration.h"

#include "core/GenreTags.h"
#include "db/Database.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

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

} // namespace

namespace GenreCuration {

AliasValidation validateAlias(const QString &alias, const QString &canonical)
{
    AliasValidation result;
    result.aliasFolded = GenreTags::folded(alias);
    result.canonicalFolded = GenreTags::folded(canonical);
    if (result.aliasFolded.isEmpty()) {
        result.error = QStringLiteral("Genre alias needs a non-empty alias");
    } else if (result.canonicalFolded.isEmpty()) {
        result.error = QStringLiteral("Genre alias needs a non-empty canonical genre");
    } else if (result.aliasFolded == result.canonicalFolded) {
        result.error = QStringLiteral("Genre alias must point at a different canonical genre");
    }
    return result;
}

QString canonicalGenreInput(Database &db, const QString &genre, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    const QString folded = GenreTags::folded(genre);
    const QString canonical = GenreTags::canonical(folded, db.genreAliases());
    if (canonical.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Genre is required");
        }
        return {};
    }
    return canonical;
}

QVector<ReportRow> buildReportRows(Database &db, int *taggedTrackTotal)
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

    QVector<ReportRow> rows;
    rows.reserve(counts.size());
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        ReportRow row;
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

    std::sort(rows.begin(), rows.end(), [](const ReportRow &a, const ReportRow &b) {
        if (a.df != b.df) {
            return a.df > b.df;
        }
        return QString::compare(a.genre, b.genre, Qt::CaseInsensitive) < 0;
    });
    return rows;
}

} // namespace GenreCuration
