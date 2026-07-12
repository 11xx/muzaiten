#include "features/QueryVectorCache.h"

#include "app/AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QVariant>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace {

// Text queries are ~2 KB each (512 float32 + key columns); the cap only
// guards against pathological growth, not normal use.
constexpr int kMaxRows = 5000;

QByteArray blobFromVector(const QVector<float> &vector)
{
    QByteArray blob;
    blob.resize(vector.size() * static_cast<qsizetype>(sizeof(quint32)));
    auto *data = reinterpret_cast<uchar *>(blob.data());
    for (qsizetype i = 0; i < vector.size(); ++i) {
        quint32 raw = 0;
        static_assert(sizeof(raw) == sizeof(float));
        std::memcpy(&raw, &vector.at(i), sizeof(raw));
        qToLittleEndian<quint32>(raw, data + i * static_cast<qsizetype>(sizeof(quint32)));
    }
    return blob;
}

QVector<float> vectorFromBlob(const QByteArray &blob, int dim)
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
        std::memcpy(&value, &raw, sizeof(value));
        if (!std::isfinite(value)) {
            return {};
        }
        vector.push_back(value);
    }
    return vector;
}

} // namespace

QueryVectorCache::QueryVectorCache(const QString &databasePath)
    : m_connectionName(QStringLiteral("muzaiten-query-cache-%1").arg(reinterpret_cast<quintptr>(this)))
{
    const QFileInfo info(databasePath);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        return;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_connectionAdded = true;
    m_db.setDatabaseName(databasePath);
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open()) {
        return;
    }
    m_open = ensureSchema();
}

QueryVectorCache::~QueryVectorCache()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    if (m_connectionAdded) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool QueryVectorCache::isOpen() const
{
    return m_open;
}

bool QueryVectorCache::ensureSchema()
{
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS query_vectors ("
        " checkpoint_sha256 TEXT NOT NULL,"
        " feature_revision TEXT NOT NULL,"
        " query_text TEXT NOT NULL,"
        " capability TEXT NOT NULL,"
        " model TEXT NOT NULL,"
        " vector_dim INTEGER NOT NULL,"
        " vector BLOB NOT NULL,"
        " created_at TEXT NOT NULL,"
        " last_used_at TEXT NOT NULL,"
        " PRIMARY KEY (checkpoint_sha256, feature_revision, query_text))"));
}

QString QueryVectorCache::defaultPath()
{
    return QDir(AppPaths::cacheDir()).filePath(QStringLiteral("semantic-query.sqlite"));
}

QString QueryVectorCache::normalizedQueryText(const QString &text)
{
    return text.simplified();
}

QVector<float> QueryVectorCache::lookup(const Identity &identity, const QString &queryText)
{
    if (!m_open || !identity.valid()) {
        return {};
    }
    const QString key = normalizedQueryText(queryText);
    if (key.isEmpty()) {
        return {};
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT vector, vector_dim, capability, model FROM query_vectors"
        " WHERE checkpoint_sha256 = ? AND feature_revision = ? AND query_text = ?"));
    query.addBindValue(identity.checkpointSha256);
    query.addBindValue(identity.featureRevision);
    query.addBindValue(key);
    if (!query.exec() || !query.next()) {
        return {};
    }
    if (query.value(1).toInt() != identity.vectorDimension
        || query.value(2).toString() != identity.capability
        || query.value(3).toString() != identity.model) {
        return {};
    }
    const QVector<float> vector = vectorFromBlob(query.value(0).toByteArray(), identity.vectorDimension);
    if (vector.isEmpty()) {
        return {};
    }

    QSqlQuery touch(m_db);
    touch.prepare(QStringLiteral(
        "UPDATE query_vectors SET last_used_at = ?"
        " WHERE checkpoint_sha256 = ? AND feature_revision = ? AND query_text = ?"));
    touch.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    touch.addBindValue(identity.checkpointSha256);
    touch.addBindValue(identity.featureRevision);
    touch.addBindValue(key);
    touch.exec();
    return vector;
}

bool QueryVectorCache::store(const Identity &identity, const QString &queryText, const QVector<float> &vector)
{
    if (!m_open || !identity.valid() || vector.size() != identity.vectorDimension) {
        return false;
    }
    const QString key = normalizedQueryText(queryText);
    if (key.isEmpty()) {
        return false;
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO query_vectors"
        " (checkpoint_sha256, feature_revision, query_text, capability, model,"
        "  vector_dim, vector, created_at, last_used_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(identity.checkpointSha256);
    query.addBindValue(identity.featureRevision);
    query.addBindValue(key);
    query.addBindValue(identity.capability);
    query.addBindValue(identity.model);
    query.addBindValue(identity.vectorDimension);
    query.addBindValue(blobFromVector(vector));
    query.addBindValue(now);
    query.addBindValue(now);
    if (!query.exec()) {
        return false;
    }
    pruneIfOversized();
    return true;
}

void QueryVectorCache::pruneIfOversized()
{
    QSqlQuery count(m_db);
    if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM query_vectors")) || !count.next()
        || count.value(0).toInt() <= kMaxRows) {
        return;
    }
    QSqlQuery prune(m_db);
    prune.prepare(QStringLiteral(
        "DELETE FROM query_vectors WHERE rowid IN ("
        " SELECT rowid FROM query_vectors ORDER BY last_used_at ASC LIMIT ?)"));
    prune.addBindValue(kMaxRows / 5);
    prune.exec();
}
