#pragma once

#include "core/Track.h"

#include <QHash>
#include <QString>
#include <QVector>

// Pure data helpers behind SemanticSearchDialog, split out for testability:
// ranking math and row formatting carry the behavior worth pinning; the
// dialog itself stays a thin shell.
namespace SemanticSearchData {

struct GroupScore {
    qint64 groupId = 0;
    double score = 0.0;
};

// Cosine ranking of the query vector against every embedding of matching
// dimension, best first, capped at `limit` (ties break toward the lower
// group id so results are deterministic).
QVector<GroupScore> rankEmbeddings(const QVector<float> &queryVector,
                                   const QHash<qint64, QVector<float>> &embeddings,
                                   int limit);

// "FLAC 16bit/44.1kHz" / "MP3 320kbps" / "OPUS"; empty when nothing is known.
QString formatQuality(const Track &track);

// "★★★★☆" from a 0-100 rating; empty when unset.
QString starText(int rating0To100);

// Four-digit year out of a date tag, empty when absent.
QString yearText(const QString &date);

} // namespace SemanticSearchData
