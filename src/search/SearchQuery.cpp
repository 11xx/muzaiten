#include "search/SearchQuery.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace Search {

namespace {

// Parse ">=96", "<=300", ">5", "<100", "=80", or bare "96" into (op, value).
// Returns false if the value part cannot be parsed as a number.
bool parseNumericToken(const QString &valueStr, CompareOp &op, qint64 &value)
{
    QString rest = valueStr;
    op = CompareOp::Ge; // default for bare number is >=

    if (rest.startsWith(QStringLiteral(">="))) {
        op   = CompareOp::Ge;
        rest = rest.mid(2);
    } else if (rest.startsWith(QStringLiteral("<="))) {
        op   = CompareOp::Le;
        rest = rest.mid(2);
    } else if (rest.startsWith(QLatin1Char('>'))) {
        op   = CompareOp::Gt;
        rest = rest.mid(1);
    } else if (rest.startsWith(QLatin1Char('<'))) {
        op   = CompareOp::Lt;
        rest = rest.mid(1);
    } else if (rest.startsWith(QLatin1Char('='))) {
        op   = CompareOp::Eq;
        rest = rest.mid(1);
    }

    bool ok = false;
    value = rest.toLongLong(&ok);
    return ok;
}

// Parse duration "m:ss" or "h:mm:ss" or bare seconds into milliseconds.
// Returns -1 on failure.
qint64 parseDurationMs(const QString &s)
{
    // Try m:ss or h:mm:ss
    const QStringList parts = s.split(QLatin1Char(':'));
    if (parts.size() == 2) {
        bool ok1, ok2;
        const qint64 m = parts[0].toLongLong(&ok1);
        const qint64 sec = parts[1].toLongLong(&ok2);
        if (ok1 && ok2) return (m * 60 + sec) * 1000;
    } else if (parts.size() == 3) {
        bool ok1, ok2, ok3;
        const qint64 h   = parts[0].toLongLong(&ok1);
        const qint64 m   = parts[1].toLongLong(&ok2);
        const qint64 sec = parts[2].toLongLong(&ok3);
        if (ok1 && ok2 && ok3) return (h * 3600 + m * 60 + sec) * 1000;
    }
    // Bare seconds
    bool ok;
    const qint64 secs = s.toLongLong(&ok);
    return ok ? secs * 1000 : -1;
}

Term parseSingleToken(const QString &raw)
{
    Term term;
    term.rawText = raw;
    QString s = raw;

    // --- operator prefixes for free/text terms ---
    if (s.startsWith(QLatin1Char('!'))) {
        term.negate = true;
        s = s.mid(1);
    }
    if (s.startsWith(QLatin1Char('\''))) {
        term.forceExact = true;
        s = s.mid(1);
    }
    if (s.startsWith(QLatin1Char('^'))) {
        term.prefixAnchor = true;
        s = s.mid(1);
    }
    if (s.endsWith(QLatin1Char('$')) && !s.isEmpty()) {
        term.suffixAnchor = true;
        s = s.chopped(1);
    }

    // --- field: prefix ---
    // Detect "word:" followed by value (allow numeric op chars)
    const int colonPos = static_cast<int>(s.indexOf(QLatin1Char(':')));
    if (colonPos > 0) {
        const QString field = s.left(colonPos).toLower();
        const QString value = s.mid(colonPos + 1);

        if (field == QStringLiteral("artist")) {
            term.kind = TermKind::ArtistText;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("albumartist") || field == QStringLiteral("aa")) {
            term.kind = TermKind::AlbumArtistText;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("album") || field == QStringLiteral("al")) {
            term.kind = TermKind::AlbumText;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("title") || field == QStringLiteral("t")) {
            term.kind = TermKind::TitleText;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("path") || field == QStringLiteral("p")) {
            term.kind = TermKind::PathText;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("file") || field == QStringLiteral("f")) {
            term.kind = TermKind::FilenameText;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("ext") || field == QStringLiteral("extension")) {
            term.kind = TermKind::Extension;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("codec")) {
            term.kind = TermKind::CodecText;
            term.text = value.toLower();
            return term;
        }
        if (field == QStringLiteral("year") || field == QStringLiteral("y")) {
            term.kind = TermKind::Year;
            if (!parseNumericToken(value, term.op, term.numericValue)) {
                // Fallback: treat as text match on date field
                term.kind = TermKind::FreeText;
                term.text = value.toLower();
            }
            return term;
        }
        if (field == QStringLiteral("rating") || field == QStringLiteral("r")) {
            term.kind = TermKind::Rating;
            if (!parseNumericToken(value, term.op, term.numericValue)) {
                return {}; // ignore malformed
            }
            return term;
        }
        if (field == QStringLiteral("dur") || field == QStringLiteral("duration")) {
            term.kind = TermKind::DurationMs;
            // Strip operator prefix before parsing duration
            QString opStr, durStr = value;
            if (durStr.startsWith(QStringLiteral(">="))) { term.op = CompareOp::Ge; durStr = durStr.mid(2); }
            else if (durStr.startsWith(QStringLiteral("<="))) { term.op = CompareOp::Le; durStr = durStr.mid(2); }
            else if (durStr.startsWith(QLatin1Char('>'))) { term.op = CompareOp::Gt; durStr = durStr.mid(1); }
            else if (durStr.startsWith(QLatin1Char('<'))) { term.op = CompareOp::Lt; durStr = durStr.mid(1); }
            else if (durStr.startsWith(QLatin1Char('='))) { term.op = CompareOp::Eq; durStr = durStr.mid(1); }
            const qint64 ms = parseDurationMs(durStr);
            if (ms < 0) return {};
            term.numericValue = ms;
            return term;
        }
        if (field == QStringLiteral("khz") || field == QStringLiteral("sr")) {
            // khz:>=96 → compare sampleRateHz >= 96000
            term.kind = TermKind::SampleRateHz;
            CompareOp op; qint64 val;
            if (parseNumericToken(value, op, val)) {
                term.op = op;
                term.numericValue = val * 1000; // khz to hz
            } else return {};
            return term;
        }
        if (field == QStringLiteral("hz")) {
            term.kind = TermKind::SampleRateHz;
            CompareOp op; qint64 val;
            if (parseNumericToken(value, op, val)) {
                term.op = op;
                term.numericValue = val; // already hz
            } else return {};
            return term;
        }
        if (field == QStringLiteral("kbps") || field == QStringLiteral("bitrate") || field == QStringLiteral("br")) {
            term.kind = TermKind::BitrateKbps;
            CompareOp op; qint64 val;
            if (parseNumericToken(value, op, val)) { term.op = op; term.numericValue = val; }
            else return {};
            return term;
        }
        if (field == QStringLiteral("ch") || field == QStringLiteral("channels")) {
            term.kind = TermKind::Channels;
            CompareOp op; qint64 val;
            if (parseNumericToken(value, op, val)) { term.op = op; term.numericValue = val; }
            else return {};
            return term;
        }
    }

    // Plain free text
    term.kind = TermKind::FreeText;
    term.text = s.toLower();
    return term;
}

} // namespace

SearchQuery SearchQuery::parse(const QString &queryString)
{
    SearchQuery q;
    if (queryString.trimmed().isEmpty()) return q;

    const QStringList tokens = queryString.split(QRegularExpression(QStringLiteral("\\s+")),
                                                  Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        const Term t = parseSingleToken(token);
        if (!t.text.isEmpty() || t.kind >= TermKind::Year) {
            q.terms.append(t);
        }
    }
    return q;
}

} // namespace Search
