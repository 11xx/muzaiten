#include "features/FeatureStore.h"

#include "indexer/DspVersion.h"

#include <QDebug>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QtEndian>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kMinSupportedSchemaVersion = 1;
constexpr int kMaxSupportedSchemaVersion = 5;
constexpr qsizetype kMaxSqlBindings = 500;

bool isSupportedSchemaVersion(int version)
{
    return version >= kMinSupportedSchemaVersion && version <= kMaxSupportedSchemaVersion;
}

QString placeholders(qsizetype count)
{
    QStringList marks;
    marks.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        marks << QStringLiteral("?");
    }
    return marks.join(QStringLiteral(", "));
}

FeatureStore::Scalars scalarsFromQuery(const QSqlQuery &query, int firstColumn)
{
    FeatureStore::Scalars scalars;
    scalars.valid = true;
    if (!query.value(firstColumn).isNull()) {
        scalars.tempoBpm = query.value(firstColumn).toDouble();
    }
    if (!query.value(firstColumn + 1).isNull()) {
        scalars.loudness = query.value(firstColumn + 1).toDouble();
    }
    if (!query.value(firstColumn + 2).isNull()) {
        scalars.energy = query.value(firstColumn + 2).toDouble();
    }
    if (!query.value(firstColumn + 3).isNull()) {
        scalars.brightness = query.value(firstColumn + 3).toDouble();
    }
    return scalars;
}

QString scalarColumnList(int schemaVersion, bool includeGroupId)
{
    QStringList columns;
    if (includeGroupId) {
        columns << QStringLiteral("content_group_id");
    }
    if (schemaVersion >= 3) {
        columns << QStringLiteral("tempo_bpm")
                << QStringLiteral("loudness_lufs")
                << QStringLiteral("energy")
                << QStringLiteral("spectral_centroid_mean_hz");
    } else {
        columns << QStringLiteral("tempo_bpm")
                << QStringLiteral("loudness")
                << QStringLiteral("energy")
                << QStringLiteral("brightness");
    }
    return columns.join(QStringLiteral(", "));
}

QVector<float> embeddingFromBlob(const QByteArray &blob, int dim)
{
    if (dim <= 0 || blob.size() != dim * static_cast<int>(sizeof(float))) {
        return {};
    }

    QVector<float> vector;
    vector.reserve(dim);
    const auto *data = reinterpret_cast<const uchar *>(blob.constData());
    for (int i = 0; i < dim; ++i) {
        const quint32 raw = qFromLittleEndian<quint32>(data + i * static_cast<int>(sizeof(quint32)));
        float value = 0.0F;
        static_assert(sizeof(value) == sizeof(raw));
        std::memcpy(&value, &raw, sizeof(value));
        if (!std::isfinite(value)) {
            return {};
        }
        vector.push_back(value);
    }
    return vector;
}

qint64 scalarCount(QSqlDatabase database, const QString &sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
}

bool hasTable(const QSqlDatabase &database, const QString &table)
{
    return database.tables().contains(table);
}

QString metaValue(QSqlDatabase database, const QString &key)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next()) {
        return {};
    }
    return query.value(0).toString();
}

} // namespace

FeatureStore::FeatureStore(const QString &path)
    : m_connectionName(QStringLiteral("muzaiten-features-%1").arg(reinterpret_cast<quintptr>(this)))
{
    if (!QFileInfo::exists(path)) {
        return;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_connectionAdded = true;
    m_db.setDatabaseName(path);
    m_db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open()) {
        qWarning("FeatureStore: failed to open %s read-only: %s",
                 qPrintable(path), qPrintable(m_db.lastError().text()));
        close();
        return;
    }

    QSqlQuery queryOnly(m_db);
    queryOnly.exec(QStringLiteral("PRAGMA query_only=ON"));

    QSqlQuery versionQuery(m_db);
    if (!versionQuery.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'schema_version'"))
        || !versionQuery.next()) {
        qWarning("FeatureStore: %s has no readable schema_version", qPrintable(path));
        close();
        return;
    }

    bool ok = false;
    const int version = versionQuery.value(0).toString().toInt(&ok);
    if (!ok || !isSupportedSchemaVersion(version)) {
        qWarning("FeatureStore: unsupported features.sqlite schema %s at %s (supported %d..%d)",
                 qPrintable(versionQuery.value(0).toString()), qPrintable(path),
                 kMinSupportedSchemaVersion, kMaxSupportedSchemaVersion);
        close();
        return;
    }
    m_schemaVersion = version;
}

FeatureStore::~FeatureStore()
{
    close();
}

void FeatureStore::close()
{
    m_schemaVersion = -1;
    if (m_db.isValid()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    if (m_connectionAdded) {
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionAdded = false;
    }
}

bool FeatureStore::isOpen() const
{
    return m_db.isOpen() && isSupportedSchemaVersion(m_schemaVersion);
}

int FeatureStore::schemaVersion() const
{
    return m_schemaVersion;
}

qint64 FeatureStore::contentGroupForPath(const QString &path) const
{
    if (!isOpen() || path.isEmpty()) {
        return -1;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT content_group_id FROM files "
        "WHERE path = ? AND content_group_id IS NOT NULL"));
    query.addBindValue(path);
    if (!query.exec() || !query.next()) {
        return -1;
    }
    return query.value(0).toLongLong();
}

QHash<QString, qint64> FeatureStore::contentGroupsForPaths(const QStringList &paths) const
{
    QHash<QString, qint64> groups;
    if (!isOpen() || paths.isEmpty()) {
        return groups;
    }

    for (qsizetype start = 0; start < paths.size(); start += kMaxSqlBindings) {
        const qsizetype count = std::min(kMaxSqlBindings, paths.size() - start);
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT path, content_group_id FROM files "
            "WHERE content_group_id IS NOT NULL AND path IN (%1)")
                          .arg(placeholders(count)));
        for (qsizetype i = 0; i < count; ++i) {
            query.addBindValue(paths.at(start + i));
        }
        if (!query.exec()) {
            continue;
        }
        while (query.next()) {
            groups.insert(query.value(0).toString(), query.value(1).toLongLong());
        }
    }
    return groups;
}

QVector<qint64> FeatureStore::contentGroupIds(int minSize) const
{
    QVector<qint64> groups;
    if (!isOpen()) {
        return groups;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT content_group_id FROM files "
        "WHERE content_group_id IS NOT NULL "
        "GROUP BY content_group_id HAVING COUNT(*) >= ? "
        "ORDER BY content_group_id"));
    query.addBindValue(std::max(1, minSize));
    if (!query.exec()) {
        return groups;
    }
    while (query.next()) {
        groups.push_back(query.value(0).toLongLong());
    }
    return groups;
}

QStringList FeatureStore::pathsInGroup(qint64 groupId) const
{
    QStringList paths;
    if (!isOpen() || groupId < 0) {
        return paths;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT path FROM files WHERE content_group_id = ? ORDER BY path"));
    query.addBindValue(groupId);
    if (!query.exec()) {
        return paths;
    }
    while (query.next()) {
        paths.push_back(query.value(0).toString());
    }
    return paths;
}

FeatureStore::Scalars FeatureStore::scalarsForGroup(qint64 groupId) const
{
    if (!isOpen() || groupId < 0) {
        return {};
    }

    // Strict freshness: only rows written by the DSP version this binary
    // expects feed the app/radio. Stale rows (older version, or an entire
    // store last written by an older binary) read as absent until the
    // indexer recomputes them — radio silently consuming old-scale scalars
    // would be worse than briefly having none.
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT %1 FROM features WHERE content_group_id = ? AND version = ?")
                      .arg(scalarColumnList(m_schemaVersion, false)));
    query.addBindValue(groupId);
    query.addBindValue(QLatin1String(Dsp::kDspVersion));
    if (!query.exec() || !query.next()) {
        return {};
    }
    return scalarsFromQuery(query, 0);
}

QHash<qint64, FeatureStore::Scalars> FeatureStore::scalarsForGroups(const QList<qint64> &groupIds) const
{
    QHash<qint64, Scalars> rows;
    if (!isOpen() || groupIds.isEmpty()) {
        return rows;
    }

    for (qsizetype start = 0; start < groupIds.size(); start += kMaxSqlBindings) {
        const qsizetype count = std::min(kMaxSqlBindings, groupIds.size() - start);
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("SELECT %1 FROM features WHERE content_group_id IN (%2)"
                                     " AND version = ?")
                          .arg(scalarColumnList(m_schemaVersion, true), placeholders(count)));
        for (qsizetype i = 0; i < count; ++i) {
            query.addBindValue(groupIds.at(start + i));
        }
        query.addBindValue(QLatin1String(Dsp::kDspVersion));
        if (!query.exec()) {
            continue;
        }
        while (query.next()) {
            rows.insert(query.value(0).toLongLong(), scalarsFromQuery(query, 1));
        }
    }
    return rows;
}

QVector<float> FeatureStore::embeddingForGroup(qint64 groupId) const
{
    if (!isOpen() || groupId < 0) {
        return {};
    }

    QSqlQuery query(m_db);
    query.prepare(m_schemaVersion >= 5
        ? QStringLiteral(
              "SELECT e.dim, e.vector FROM embeddings e JOIN semantic_generations g"
              " ON g.id = e.generation_id AND g.active <> 0 WHERE e.content_group_id = ?")
        : QStringLiteral("SELECT dim, vector FROM embeddings WHERE content_group_id = ?"));
    query.addBindValue(groupId);
    if (!query.exec() || !query.next()) {
        return {};
    }
    return embeddingFromBlob(query.value(1).toByteArray(), query.value(0).toInt());
}

QHash<qint64, QVector<float>> FeatureStore::embeddingsForGroups(const QList<qint64> &groupIds) const
{
    QHash<qint64, QVector<float>> rows;
    if (!isOpen() || groupIds.isEmpty()) {
        return rows;
    }

    for (qsizetype start = 0; start < groupIds.size(); start += kMaxSqlBindings) {
        const qsizetype count = std::min(kMaxSqlBindings, groupIds.size() - start);
        QSqlQuery query(m_db);
        query.prepare(m_schemaVersion >= 5
            ? QStringLiteral(
                  "SELECT e.content_group_id, e.dim, e.vector FROM embeddings e"
                  " JOIN semantic_generations g ON g.id = e.generation_id AND g.active <> 0"
                  " WHERE e.content_group_id IN (%1)")
                  .arg(placeholders(count))
            : QStringLiteral(
                  "SELECT content_group_id, dim, vector FROM embeddings "
                  "WHERE content_group_id IN (%1)")
                  .arg(placeholders(count)));
        for (qsizetype i = 0; i < count; ++i) {
            query.addBindValue(groupIds.at(start + i));
        }
        if (!query.exec()) {
            continue;
        }
        while (query.next()) {
            QVector<float> embedding = embeddingFromBlob(query.value(2).toByteArray(), query.value(1).toInt());
            if (!embedding.isEmpty()) {
                rows.insert(query.value(0).toLongLong(), embedding);
            }
        }
    }
    return rows;
}

QList<QPair<qint64, double>> FeatureStore::neighborsOfGroup(qint64 groupId, int limit) const
{
    QList<QPair<qint64, double>> rows;
    if (!isOpen() || groupId < 0 || limit <= 0) {
        return rows;
    }

    QSqlQuery query(m_db);
    query.prepare(m_schemaVersion >= 5
        ? QStringLiteral(
              "SELECT n.neighbor_group_id, n.cosine FROM track_neighbors n"
              " JOIN semantic_generations g ON g.id = n.generation_id AND g.active <> 0"
              " WHERE n.content_group_id = ? AND n.algorithm_revision = 'cosine-blockwise-v1'"
              " AND n.top_k >= ? ORDER BY n.rank LIMIT ?")
        : QStringLiteral(
              "SELECT neighbor_group_id, cosine FROM track_neighbors "
              "WHERE content_group_id = ? ORDER BY rank LIMIT ?"));
    query.addBindValue(groupId);
    if (m_schemaVersion >= 5) {
        query.addBindValue(limit);
    }
    query.addBindValue(limit);
    if (!query.exec()) {
        return rows;
    }
    while (query.next()) {
        rows.push_back({query.value(0).toLongLong(), query.value(1).toDouble()});
    }
    return rows;
}

FeatureStore::SemanticGeneration FeatureStore::activeSemanticGeneration() const
{
    SemanticGeneration generation;
    if (!isOpen() || m_schemaVersion < 5 || !hasTable(m_db, QStringLiteral("semantic_generations"))) {
        return generation;
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT id, capability, model, checkpoint_sha256, feature_revision, vector_dim"
            " FROM semantic_generations WHERE active <> 0 ORDER BY id DESC LIMIT 1"))
        || !query.next()) {
        return generation;
    }
    generation.id = query.value(0).toLongLong();
    generation.capability = query.value(1).toString();
    generation.model = query.value(2).toString();
    generation.checkpointSha256 = query.value(3).toString();
    generation.featureRevision = query.value(4).toString();
    generation.vectorDimension = query.value(5).toInt();
    return generation;
}

FeatureStore::Status FeatureStore::status() const
{
    Status result;
    if (!isOpen()) {
        return result;
    }

    QSqlQuery filesQuery(m_db);
    if (filesQuery.exec(QStringLiteral(
            "SELECT COUNT(*),"
            " SUM(CASE WHEN status = 'ok' THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN status <> 'ok' THEN 1 ELSE 0 END)"
            " FROM files"))
        && filesQuery.next()) {
        result.files = filesQuery.value(0).toLongLong();
        result.ok = filesQuery.value(1).toLongLong();
        result.failed = filesQuery.value(2).toLongLong();
    }
    result.groups = scalarCount(m_db, QStringLiteral("SELECT COUNT(*) FROM content_groups"));
    result.featured = scalarCount(m_db, QStringLiteral("SELECT COUNT(*) FROM features"));
    QSqlQuery freshQuery(m_db);
    freshQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM features WHERE version = ?"));
    freshQuery.addBindValue(QLatin1String(Dsp::kDspVersion));
    if (freshQuery.exec() && freshQuery.next()) {
        result.featuredFresh = freshQuery.value(0).toLongLong();
    }
    result.featuredStale = result.featured - result.featuredFresh;
    result.dspVersion = metaValue(m_db, QStringLiteral("dsp_version"));
    result.expectedDspVersion = QLatin1String(Dsp::kDspVersion);
    if (hasTable(m_db, QStringLiteral("embeddings"))) {
        result.embeddedGroups = scalarCount(
            m_db,
            m_schemaVersion >= 5
                ? QStringLiteral(
                      "SELECT COUNT(DISTINCT e.content_group_id) FROM embeddings e"
                      " JOIN semantic_generations g ON g.id = e.generation_id AND g.active <> 0")
                : QStringLiteral("SELECT COUNT(DISTINCT content_group_id) FROM embeddings"));
        QSqlQuery embeddingQuery(m_db);
        const QString embeddingSql = m_schemaVersion >= 5
            ? QStringLiteral(
                  "SELECT g.model, g.feature_revision, COUNT(*) AS n FROM embeddings e"
                  " JOIN semantic_generations g ON g.id = e.generation_id AND g.active <> 0"
                  " GROUP BY g.id LIMIT 1")
            : QStringLiteral(
                  "SELECT model, version, COUNT(*) AS n FROM embeddings "
                  "GROUP BY model, version ORDER BY n DESC, model, version LIMIT 1");
        if (embeddingQuery.exec(embeddingSql)
            && embeddingQuery.next()) {
            result.embeddingModel = embeddingQuery.value(0).toString();
            result.embeddingVersion = embeddingQuery.value(1).toString();
        }
    }
    if (hasTable(m_db, QStringLiteral("track_neighbors"))) {
        result.neighborRows = scalarCount(
            m_db,
            m_schemaVersion >= 5
                ? QStringLiteral(
                      "SELECT COUNT(*) FROM track_neighbors n JOIN semantic_generations g"
                      " ON g.id = n.generation_id AND g.active <> 0"
                      " WHERE n.algorithm_revision = 'cosine-blockwise-v1'")
                : QStringLiteral("SELECT COUNT(*) FROM track_neighbors"));
    }
    return result;
}
