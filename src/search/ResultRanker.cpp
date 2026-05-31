#include "search/ResultRanker.h"

#include "core/MusicSort.h"
#include "core/Track.h"

#include <QSet>

#include <algorithm>
#include <numeric>

namespace Search {

namespace {

bool isLosslessCodec(const QString &codec)
{
    static const QSet<QString> kLossless = {
        QStringLiteral("flac"), QStringLiteral("alac"), QStringLiteral("wav"),
        QStringLiteral("aiff"), QStringLiteral("aif"),  QStringLiteral("ape"),
        QStringLiteral("wv"),   QStringLiteral("tak"),  QStringLiteral("dsf"),
        QStringLiteral("dff"),  QStringLiteral("tta"),  QStringLiteral("shn"),
    };
    return kLossless.contains(codec.toLower());
}

// Build the minimal Track needed for MusicSort::compareField.
Track toTrack(const SearchRecord &r)
{
    Track t;
    t.title            = r.title;
    t.artistName       = r.artistName;
    t.albumArtistName  = r.albumArtistName;
    t.albumTitle       = r.albumTitle;
    t.date             = r.date;
    t.originalDate     = r.date;   // SearchRecord carries a single date string
    t.trackNumber      = r.trackNumber;
    t.discNumber       = r.discNumber;
    t.durationMs       = r.durationMs;
    t.effectiveRating0To100 = r.rating0To100;
    t.fileMtime        = r.fileMtime;
    t.fileSize         = r.fileSize;
    t.filename         = r.filename;
    return t;
}

int cmpInt(qint64 a, qint64 b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

} // namespace

int audioQualityScore(const SearchRecord &rec)
{
    int s = 0;
    if (isLosslessCodec(rec.codec)) s += 4'000'000;
    s += rec.bitDepth * 100'000;       // 0 today; dominates within lossless once scanned
    s += rec.sampleRateHz / 100;       // 96000 -> 960, 44100 -> 441
    s += rec.bitrateKbps;              // differentiates lossy
    s += rec.channels * 50;
    return s;
}

void ResultRanker::sort(QVector<ScoredResult> &results) const
{
    if (results.size() < 2) return;

    QVector<RankRule> rules;
    for (const RankRule &r : m_config.rules) {
        if (r.enabled) rules.push_back(r);
    }
    if (rules.isEmpty()) return; // keep the engine's relevance order

    const bool needTrack = std::any_of(rules.begin(), rules.end(), [](const RankRule &r) {
        return r.kind == RankKind::LibraryOrder || r.kind == RankKind::MusicField;
    });
    const bool needQuality = std::any_of(rules.begin(), rules.end(), [](const RankRule &r) {
        return r.kind == RankKind::AudioQuality;
    });

    const int n = static_cast<int>(results.size());
    QVector<Track> tracks;
    QVector<int> quality;
    if (needTrack) {
        tracks.resize(n);
        for (int i = 0; i < n; ++i) tracks[i] = toTrack(results[i].rec);
    }
    if (needQuality) {
        quality.resize(n);
        for (int i = 0; i < n; ++i) quality[i] = audioQualityScore(results[i].rec);
    }

    auto ruleCmp = [&](const RankRule &rule, int a, int b) -> int {
        const bool desc = rule.dir == MusicSort::SortDirection::Descending;
        switch (rule.kind) {
        case RankKind::Relevance: {
            int raw = cmpInt(results[a].score, results[b].score);
            return desc ? -raw : raw;
        }
        case RankKind::AudioQuality: {
            int raw = cmpInt(quality[a], quality[b]);
            return desc ? -raw : raw;
        }
        case RankKind::PreferredDirectory: {
            const QString p = rule.param.toLower();
            const int ha = (!p.isEmpty() && results[a].rec.normPath.startsWith(p)) ? 1 : 0;
            const int hb = (!p.isEmpty() && results[b].rec.normPath.startsWith(p)) ? 1 : 0;
            int raw = cmpInt(ha, hb);
            return desc ? -raw : raw; // desc => preferred (hit) first
        }
        case RankKind::MusicField: {
            int raw = MusicSort::compareField(rule.field, tracks[a], tracks[b]);
            return desc ? -raw : raw;
        }
        case RankKind::LibraryOrder: {
            int raw = MusicSort::compareField(rule.field, tracks[a], tracks[b]);
            if (desc) raw = -raw;
            if (raw != 0) return raw;
            for (MusicSort::SortField f : MusicSort::tiebreakChainFor(rule.field)) {
                int t = MusicSort::compareField(f, tracks[a], tracks[b]);
                if (t != 0) return t;
            }
            return 0;
        }
        }
        return 0;
    };

    QVector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        for (const RankRule &rule : rules) {
            const int c = ruleCmp(rule, a, b);
            if (c != 0) return c < 0;
        }
        return false; // full tie: stable_sort preserves engine (relevance) order
    });

    QVector<ScoredResult> reordered;
    reordered.reserve(n);
    for (int idx : order) reordered.push_back(std::move(results[idx]));
    results = std::move(reordered);
}

} // namespace Search
