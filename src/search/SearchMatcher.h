#pragma once

#include "search/SearchQuery.h"
#include "search/fold/Fold.h"

#include <QString>
#include <QStringView>
#include <array>
#include <QVector>

namespace Search {

struct SearchRecord;

enum class MatchFieldRole {
    Free,
    Title,
    Artist,
    AlbumArtist,
    Album,
    Path,
    Filename,
    Codec,
    Extension,   // the name on disk; `codec:` and `ext:` answer different questions
};

struct MatchField {
    MatchFieldRole role = MatchFieldRole::Free;
    QString text;
    QString normText;
    int weight = 100;
};

// Build a MatchField whose normalized form comes from the shared fold engine, so
// the in-panel document filter matches by the same diacritic/script/romaji rules
// and alternate readings as the library index. `text` keeps its original casing
// for display.
inline MatchField makeField(MatchFieldRole role, const QString &text, int weight)
{
    return {role, text, Fold::foldSegments(text), weight};
}

struct MatchNumeric {
    TermKind kind = TermKind::Year;
    qint64 value = 0;
};

struct MatchDocument {
    int row = -1;
    QVector<MatchField> fields;
    QVector<MatchNumeric> numeric;
};

struct PanelMatch {
    int row = -1;
    int score = 0;
};

int matchDocument(const MatchDocument &doc, const SearchQuery &query, bool fuzzyMode);
QVector<PanelMatch> matchDocumentsInDisplayOrder(const QVector<MatchDocument> &docs,
                                                  const SearchQuery &query,
                                                  bool fuzzyMode);

inline int firstPanelMatchRow(const QVector<MatchDocument> &docs, const QString &text)
{
    if (text.trimmed().isEmpty()) return -1;
    const auto matches = matchDocumentsInDisplayOrder(docs, SearchQuery::parse(text), false);
    return matches.isEmpty() ? -1 : matches.first().row;
}

// Hot-path matcher for the library index. This keeps SearchIndex from building
// per-record document vectors while sharing the same term/scoring rules.
int matchSearchRecord(const SearchRecord &rec, const SearchQuery &query, bool fuzzyMode);

// Character-presence masks for the six folded record fields.
enum NormSlot { NormTitle, NormArtist, NormAlbumArtist, NormAlbum, NormFilename, NormPath, NormSlotCount };
using NormMasks = std::array<quint64, NormSlotCount>;
quint64 normCharacterMask(QStringView text);
NormMasks recordNormMasks(const SearchRecord &rec);
QVector<quint64> termCharacterMasks(const SearchQuery &query);
int matchSearchRecordBound(const SearchRecord &rec, const NormMasks &masks,
                           const SearchQuery &query, const QVector<quint64> &termMasks,
                           bool fuzzyMode, bool *exact);
int scoreSearchRecord(const SearchRecord &rec, const NormMasks &masks,
                      const SearchQuery &query, const QVector<quint64> &termMasks,
                      bool fuzzyMode);

} // namespace Search
