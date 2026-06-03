#include "db/SettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QVariant>

namespace {
constexpr int kSchemaVersion = 1;
}

SettingsStore::SettingsStore(const QString &path)
    : m_connectionName(QStringLiteral("muzaiten-state-%1").arg(reinterpret_cast<quintptr>(this)))
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));

    QSqlQuery create(m_db);
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL)"));
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"));

    QSqlQuery version(m_db);
    version.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES('schemaVersion', ?) ON CONFLICT(key) DO NOTHING"));
    version.addBindValue(QString::number(kSchemaVersion));
    version.exec();
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
    return m_db.isOpen();
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
