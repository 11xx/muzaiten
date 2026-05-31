#include "search/Exclusion.h"

#include "search/SearchRecord.h"

#include <QRegularExpression>

namespace Search {

ExcludeMatcher::ExcludeMatcher(const ExcludeRule &rule)
    : m_scope(rule.scope)
{
    QString g = rule.glob.trimmed();
    if (g.isEmpty()) {
        return;
    }

    const QString lower = g.toLower();
    const bool hasInterior = [&] {
        // Wildcards anywhere other than the leading/trailing position force regex.
        const QString inner = lower.mid(1, lower.length() - 2);
        return inner.contains(QLatin1Char('*')) || inner.contains(QLatin1Char('?'))
            || lower.contains(QLatin1Char('?'));
    }();

    if (hasInterior) {
        m_kind = Kind::Regex;
        m_regex = QRegularExpression::fromWildcard(
            g, Qt::CaseInsensitive, QRegularExpression::UnanchoredWildcardConversion);
        // Wildcard conversion anchors the whole string; we want substring-style
        // containment, so rebuild unanchored when there are no anchoring needs.
        m_valid = m_regex.isValid();
        return;
    }

    const bool lead  = lower.startsWith(QLatin1Char('*'));
    const bool trail = lower.endsWith(QLatin1Char('*'));
    QString core = lower;
    if (trail) core.chop(1);
    if (lead && !core.isEmpty()) core = core.mid(1);

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
    case Kind::Prefix:    return normField.startsWith(m_needle);
    case Kind::Suffix:    return normField.endsWith(m_needle);
    case Kind::Regex:     return m_regex.match(normField).hasMatch();
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
