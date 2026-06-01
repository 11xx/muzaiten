#pragma once

#include "search/SearchQuery.h"

#include <QString>
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
};

struct MatchField {
    MatchFieldRole role = MatchFieldRole::Free;
    QString text;
    QString normText;
    int weight = 100;
};

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

// Hot-path matcher for the library index. This keeps SearchIndex from building
// per-record document vectors while sharing the same term/scoring rules.
int matchSearchRecord(const SearchRecord &rec, const SearchQuery &query, bool fuzzyMode);

} // namespace Search
