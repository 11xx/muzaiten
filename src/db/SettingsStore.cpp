#include "db/SettingsStore.h"
#include "db/SqlUtil.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

SettingsStore::SettingsStore(const QString &path)
    : m_connectionName(QStringLiteral("muzaiten-state-%1").arg(reinterpret_cast<quintptr>(this)))
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));

    if (!SqlUtil::validateSchemaVersion(m_db, QStringLiteral("meta"), QStringLiteral("schemaVersion"), currentSchemaVersion, &m_lastError)) return;
    SqlUtil::Savepoint migration(m_db, &m_lastError);
    if (!migration.active()) return;

    QSqlQuery create(m_db);
    if (!create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL)"))
        || !create.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"))) {
        m_lastError = create.lastError().text();
        return;
    }

    QSqlQuery version(m_db);
    version.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES('schemaVersion', ?) ON CONFLICT(key) DO NOTHING"));
    version.addBindValue(QString::number(currentSchemaVersion));
    if (!version.exec()) {
        m_lastError = version.lastError().text();
        return;
    }
    if (!create.exec(QStringLiteral("SELECT key, value, updated_at FROM settings LIMIT 0"))) {
        m_lastError = create.lastError().text();
        return;
    }
    create.finish();
    m_ready = migration.commit();
}

SettingsStore::~SettingsStore()
{
    if (m_db.isValid()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool SettingsStore::isOpen() const
{
    return m_ready && m_db.isOpen();
}

void SettingsStore::releaseCacheMemory()
{
    if (!m_db.isOpen()) {
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA shrink_memory"));
}

QString SettingsStore::setting(const QString &key, const QString &fallback) const
{
    if (!m_db.isOpen()) {
        return fallback;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next()) {
        return fallback;
    }
    return query.value(0).toString();
}

bool SettingsStore::setSetting(const QString &key, const QString &value)
{
    if (!m_db.isOpen()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO settings(key, value, updated_at) VALUES(?, ?, datetime('now')) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at"));
    query.addBindValue(key);
    query.addBindValue(value);
    return query.exec();
}

bool SettingsStore::removeSetting(const QString &key)
{
    if (!m_db.isOpen()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
    query.addBindValue(key);
    return query.exec();
}
