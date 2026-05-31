#include "search/RankConfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Search {

QString RankConfig::rankKindToString(RankKind k)
{
    switch (k) {
    case RankKind::Relevance:          return QStringLiteral("relevance");
    case RankKind::AudioQuality:       return QStringLiteral("audioQuality");
    case RankKind::PreferredDirectory: return QStringLiteral("preferredDirectory");
    case RankKind::LibraryOrder:       return QStringLiteral("libraryOrder");
    case RankKind::MusicField:         return QStringLiteral("musicField");
    }
    return QStringLiteral("relevance");
}

RankKind RankConfig::rankKindFromString(const QString &s, RankKind fallback)
{
    if (s == QStringLiteral("relevance"))          return RankKind::Relevance;
    if (s == QStringLiteral("audioQuality"))       return RankKind::AudioQuality;
    if (s == QStringLiteral("preferredDirectory")) return RankKind::PreferredDirectory;
    if (s == QStringLiteral("libraryOrder"))       return RankKind::LibraryOrder;
    if (s == QStringLiteral("musicField"))         return RankKind::MusicField;
    return fallback;
}

namespace {

QString dirToString(MusicSort::SortDirection d)
{
    return d == MusicSort::SortDirection::Ascending ? QStringLiteral("asc") : QStringLiteral("desc");
}

MusicSort::SortDirection dirFromString(const QString &s)
{
    return s == QStringLiteral("asc") ? MusicSort::SortDirection::Ascending
                                      : MusicSort::SortDirection::Descending;
}

QString scopeToString(ExcludeScope s)
{
    return s == ExcludeScope::AnyField ? QStringLiteral("any") : QStringLiteral("path");
}

ExcludeScope scopeFromString(const QString &s)
{
    return s == QStringLiteral("any") ? ExcludeScope::AnyField : ExcludeScope::Path;
}

} // namespace

RankConfig RankConfig::defaultConfig()
{
    RankConfig c;
    c.rules.push_back({RankKind::Relevance, MusicSort::SortField::AlbumArtist, {},
                       MusicSort::SortDirection::Descending, true});
    c.rules.push_back({RankKind::AudioQuality, MusicSort::SortField::AlbumArtist, {},
                       MusicSort::SortDirection::Descending, true});
    c.rules.push_back({RankKind::LibraryOrder, MusicSort::SortField::AlbumArtist, {},
                       MusicSort::SortDirection::Ascending, true});
    return c;
}

QJsonObject RankConfig::toJson() const
{
    QJsonArray ruleArr;
    for (const RankRule &r : rules) {
        QJsonObject o;
        o.insert(QStringLiteral("kind"), rankKindToString(r.kind));
        o.insert(QStringLiteral("field"), MusicSort::sortFieldToString(r.field));
        o.insert(QStringLiteral("param"), r.param);
        o.insert(QStringLiteral("dir"), dirToString(r.dir));
        o.insert(QStringLiteral("enabled"), r.enabled);
        ruleArr.append(o);
    }
    QJsonArray exclArr;
    for (const ExcludeRule &e : excludes) {
        QJsonObject o;
        o.insert(QStringLiteral("glob"), e.glob);
        o.insert(QStringLiteral("scope"), scopeToString(e.scope));
        exclArr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("rules"), ruleArr);
    root.insert(QStringLiteral("excludes"), exclArr);
    return root;
}

RankConfig RankConfig::fromJson(const QJsonObject &obj)
{
    RankConfig c;
    for (const QJsonValue &v : obj.value(QStringLiteral("rules")).toArray()) {
        const QJsonObject o = v.toObject();
        RankRule r;
        r.kind = rankKindFromString(o.value(QStringLiteral("kind")).toString(), RankKind::Relevance);
        r.field = MusicSort::sortFieldFromString(o.value(QStringLiteral("field")).toString(),
                                                 MusicSort::SortField::AlbumArtist);
        r.param = o.value(QStringLiteral("param")).toString();
        r.dir = dirFromString(o.value(QStringLiteral("dir")).toString());
        r.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        c.rules.push_back(r);
    }
    for (const QJsonValue &v : obj.value(QStringLiteral("excludes")).toArray()) {
        const QJsonObject o = v.toObject();
        ExcludeRule e;
        e.glob = o.value(QStringLiteral("glob")).toString();
        e.scope = scopeFromString(o.value(QStringLiteral("scope")).toString());
        if (!e.glob.trimmed().isEmpty()) c.excludes.push_back(e);
    }
    return c;
}

RankConfig RankConfig::fromJsonString(const QString &json)
{
    if (json.trimmed().isEmpty()) return defaultConfig();
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return defaultConfig();
    const RankConfig c = fromJson(doc.object());
    return c.rules.isEmpty() && c.excludes.isEmpty() ? defaultConfig() : c;
}

QString RankConfig::toJsonString() const
{
    return QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

} // namespace Search
