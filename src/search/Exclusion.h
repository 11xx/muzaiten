#pragma once

// Result exclusion: glob patterns that drop matching records during the engine
// filter phase (before relevance scoring), so excluded items are skipped early
// and never consume the result cap.
//
// Each rule is a glob (*, ?) plus a scope. Globs are auto-lowered to the cheapest
// matcher at compile time: a bare word or `*word*` becomes a substring test,
// `word*` a prefix, `*word` a suffix, and anything with interior `*`/`?` falls
// back to a compiled wildcard regex.

#include <QMetaType>
#include <QRegularExpression>
#include <QString>
#include <QVector>

namespace Search {

struct SearchRecord;

enum class ExcludeScope {
    Path,      // match the file path / directory only
    AnyField,  // match path + title + artist + album-artist + album
};

struct ExcludeRule {
    QString      glob;
    ExcludeScope scope = ExcludeScope::Path;
};

class ExcludeMatcher {
public:
    explicit ExcludeMatcher(const ExcludeRule &rule);

    // True if the record should be excluded from results.
    bool matches(const SearchRecord &rec) const;

    bool isValid() const { return m_valid; }

private:
    enum class Kind { Substring, Prefix, Suffix, Regex };

    bool matchField(const QString &normField) const;

    ExcludeScope        m_scope = ExcludeScope::Path;
    Kind                m_kind  = Kind::Substring;
    QString             m_needle;   // lowercased, for substring/prefix/suffix
    QRegularExpression  m_regex;    // for Kind::Regex (case-insensitive)
    bool                m_valid = false;
};

using ExclusionSet = QVector<ExcludeMatcher>;

ExclusionSet compileExcludes(const QVector<ExcludeRule> &rules);

} // namespace Search

Q_DECLARE_METATYPE(Search::ExcludeRule)
Q_DECLARE_METATYPE(QVector<Search::ExcludeRule>)
