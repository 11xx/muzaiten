#pragma once

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>

namespace TrackScorer { struct Candidate; }

struct TasteProfile {
    struct Quantiles {
        double q25 = 0.0;
        double q50 = 0.0;
        double q75 = 0.0;
    };
    QMap<QString, double> genres;
    QMap<QString, Quantiles> features;
    QMap<QString, double> decades;
};

namespace TasteProfiles {

// Bounds an exchange profile both as received and in its normalized form.
inline constexpr qsizetype MaximumBytes = 64 * 1024;
inline constexpr int MinimumSupport = 5;
inline constexpr double MaximumScore = 3.0;
// Reads and writes of the seed store share these bounds: every store this module
// writes loads back, and a write that would break either bound changes nothing.
inline constexpr int MaximumSeeds = 32;
inline constexpr qsizetype MaximumStoreBytes = 4 * 1024 * 1024;

QString contentsDescription();
bool validName(const QString &name);
bool parse(const QByteArray &bytes, TasteProfile *profile, QString *error);
bool encode(const TasteProfile &profile, QByteArray *bytes, QString *error);
bool read(const QString &path, TasteProfile *profile, QString *error);
bool write(const QString &path, const QByteArray &preview, bool replace, QString *error);
bool aggregate(const QVector<TrackScorer::Candidate> &tracks, bool optIn, bool includeDecades,
               TasteProfile *profile, QString *error);
bool fromLibrary(const QString &libraryPath, const QString &featuresPath, bool optIn,
                 bool includeDecades, TasteProfile *profile, QString *error);
// Merge alias-canonicalized labels, drop invalid labels and normalize retained shares.
TasteProfile withAliases(const TasteProfile &profile, const QHash<QString, QString> &aliases);
double similarity(const TasteProfile &profile, const TrackScorer::Candidate &candidate);

// Names are local seed labels and are absent from exchange files. Import does
// not select a seed; sessions own a copy once the listener selects one.
QString storagePath();
bool load(QMap<QString, TasteProfile> *profiles, QString *error);
bool importProfile(const QString &name, const TasteProfile &profile, bool replace,
                   bool dryRun, QString *error);
bool remove(const QString &name, QString *error);

} // namespace TasteProfiles
