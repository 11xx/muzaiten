#pragma once

// Search query grammar.
//
// A query string is split on whitespace into terms.  All terms AND together,
// order-independent.  Terms are of two kinds:
//
//   Free terms — matched against the combined text haystack (title + artist +
//   album-artist + album + filename + path + date) or against a specific field
//   when prefixed with "field:".  fzf-style operators are supported on free
//   terms:
//     ^word   — prefix anchor (must start the field)
//     word$   — suffix anchor
//     !word   — negate (must NOT match)
//     'word   — force exact match even when fuzzy mode is on
//
//   Field tokens — constrain a specific attribute:
//     artist:      albumartist:    album:    title:    path:    file:
//     ext:flac     (extension, no comparison op)
//     codec:       (same as ext but from the codec column)
//     year:>=2000  kbps:>320   khz:>=96   hz:>=96000
//     ch:2         dur:>3:30   rating:>=80
//
//   Numeric comparison operators: = > < >= <=
//   If omitted, defaults to "==" (exact for ext/codec, ">=" for numeric).

#include <QString>
#include <QVector>

namespace Search {

enum class TermKind {
    FreeText,        // match across the default haystack
    // field-restricted text
    ArtistText,
    AlbumArtistText,
    AlbumText,
    TitleText,
    PathText,
    FilenameText,
    CodecText,       // codec: free-text match
    // numeric tokens
    Year,
    Rating,          // 0–100
    DurationMs,      // compared in milliseconds (user enters m:ss or seconds)
    SampleRateHz,    // khz: / hz:
    BitrateKbps,     // kbps: / bitrate:
    Channels,        // ch:
    Extension,       // ext:  (text match on codec/extension field)
};

enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };

struct Term {
    TermKind  kind   = TermKind::FreeText;
    CompareOp op     = CompareOp::Eq;    // for numeric tokens; Eq = contains for text
    QString   text;          // lowercased pattern for matching
    QString   rawText;       // original casing (for display)
    bool      negate      = false;  // ! prefix
    bool      prefixAnchor= false;  // ^ prefix
    bool      suffixAnchor= false;  // $ suffix
    bool      forceExact  = false;  // ' prefix (no-op when not in fuzzy mode)
    qint64    numericValue= 0;      // parsed value for numeric tokens
};

struct SearchQuery {
    QVector<Term> terms;
    bool isEmpty() const { return terms.isEmpty(); }

    // Parse a query string into terms.
    static SearchQuery parse(const QString &queryString);
};

} // namespace Search
