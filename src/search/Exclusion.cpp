#include "search/Exclusion.h"

#include "search/SearchRecord.h"
#include "search/fold/Fold.h"

#include <QRegularExpression>

namespace Search {

enum class GlobTokenKind { Literal, Star, Question };

struct GlobToken {
    GlobTokenKind kind;
    QString text;
};

static QVector<GlobToken> foldGlob(const QString &glob)
{
    QVector<GlobToken> tokens;
    QString literalRun;
    const auto appendLiteralRun = [&] {
        if (!literalRun.isEmpty()) {
            const QString folded = Fold::foldText(literalRun);
            if (!folded.isEmpty()) {
                tokens.push_back({GlobTokenKind::Literal, folded});
            }
            literalRun.clear();
        }
    };

    for (const QChar c : glob) {
        if (c == QLatin1Char('*') || c == QLatin1Char('?')) {
            appendLiteralRun();
            tokens.push_back({c == QLatin1Char('*') ? GlobTokenKind::Star : GlobTokenKind::Question, {}});
        } else {
            literalRun += c;
        }
    }
    appendLiteralRun();
    return tokens;
}

static QString globRegex(const QVector<GlobToken> &tokens)
{
    QString pattern;
    for (const GlobToken &token : tokens) {
        if (token.kind == GlobTokenKind::Literal) {
            pattern += QRegularExpression::escape(token.text);
        } else {
            const QString wildcard = token.kind == GlobTokenKind::Star
                ? QStringLiteral("*") : QStringLiteral("?");
            pattern += QRegularExpression::wildcardToRegularExpression(
                wildcard, QRegularExpression::UnanchoredWildcardConversion);
        }
    }
    return pattern;
}

ExcludeMatcher::ExcludeMatcher(const ExcludeRule &rule)
    : m_scope(rule.scope)
{
    QString g = rule.glob.trimmed();
    if (g.isEmpty()) {
        return;
    }

    const QVector<GlobToken> tokens = foldGlob(g);
    bool hasQuestion = false;
    bool hasInterior = false;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        const GlobTokenKind kind = tokens.at(i).kind;
        hasQuestion |= kind == GlobTokenKind::Question;
        hasInterior |= kind != GlobTokenKind::Literal && i > 0 && i + 1 < tokens.size();
    }
    hasInterior |= hasQuestion;

    if (hasInterior) {
        m_kind = Kind::Regex;
        m_regex = QRegularExpression(globRegex(tokens), QRegularExpression::CaseInsensitiveOption);
        m_valid = m_regex.isValid();
        return;
    }

    const bool lead = !tokens.isEmpty() && tokens.first().kind == GlobTokenKind::Star;
    const bool trail = !tokens.isEmpty() && tokens.last().kind == GlobTokenKind::Star;
    const qsizetype first = lead ? 1 : 0;
    const qsizetype last = tokens.size() - (trail ? 1 : 0);
    QString core;
    for (qsizetype i = first; i < last; ++i) {
        core += tokens.at(i).text;
    }

    if (core.isEmpty()) {
        return;  // pattern was just "*" — matches everything; treat as invalid/no-op
    }

    if (lead && trail) {
        m_kind = Kind::Substring;
    } else if (trail) {
        m_kind = Kind::Prefix;
    } else if (lead) {
        m_kind = Kind::Suffix;
    } else {
        // Bare word (no wildcards): treat as substring — the intuitive "exclude
        // anything containing this".
        m_kind = Kind::Substring;
    }
    m_needle = core;
    m_valid = true;
}

bool ExcludeMatcher::matchField(const QString &normField) const
{
    if (normField.isEmpty()) return false;
    switch (m_kind) {
    case Kind::Substring: return normField.contains(m_needle);
    case Kind::Prefix:
    case Kind::Suffix:
    case Kind::Regex:
        // A norm holds one segment per folded form; a glob matches a form, never
        // a span across two of them.
        for (const QStringView segment : QStringView(normField).tokenize(Fold::kSegmentSeparator)) {
            const bool hit = m_kind == Kind::Prefix ? segment.startsWith(m_needle)
                           : m_kind == Kind::Suffix ? segment.endsWith(m_needle)
                                                    : m_regex.matchView(segment).hasMatch();
            if (hit) return true;
        }
        return false;
    }
    return false;
}

bool ExcludeMatcher::matches(const SearchRecord &rec) const
{
    if (!m_valid) return false;
    if (matchField(rec.normPath)) return true;
    if (m_scope == ExcludeScope::AnyField) {
        return matchField(rec.normTitle)
            || matchField(rec.normArtist)
            || matchField(rec.normAlbumArtist)
            || matchField(rec.normAlbum);
    }
    return false;
}

ExclusionSet compileExcludes(const QVector<ExcludeRule> &rules)
{
    ExclusionSet set;
    set.reserve(rules.size());
    for (const ExcludeRule &rule : rules) {
        ExcludeMatcher m(rule);
        if (m.isValid()) set.push_back(std::move(m));
    }
    return set;
}

} // namespace Search
