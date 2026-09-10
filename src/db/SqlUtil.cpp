#include "db/SqlUtil.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <QUuid>

namespace SqlUtil {

Savepoint::Savepoint(QSqlDatabase database, QString *error)
    : m_database(std::move(database))
    , m_name(QStringLiteral("sp_") + QUuid::createUuid().toString(QUuid::Id128))
    , m_error(error)
{
    QSqlQuery query(m_database);
    m_active = query.exec(QStringLiteral("SAVEPOINT ") + m_name);
    if (!m_active && m_error != nullptr) *m_error = query.lastError().text();
}

Savepoint::~Savepoint()
{
    if (m_active) {
        QSqlQuery query(m_database);
        query.exec(QStringLiteral("ROLLBACK TO ") + m_name);
        query.exec(QStringLiteral("RELEASE ") + m_name);
    }
}

bool Savepoint::commit()
{
    if (!m_active) return false;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("RELEASE ") + m_name)) {
        if (m_error != nullptr) *m_error = query.lastError().text();
        return false;
    }
    m_active = false;
    return true;
}

bool validateSchemaVersion(QSqlDatabase database, const QString &table,
                           const QString &key, int supported, QString *error)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?"));
    query.addBindValue(table);
    if (!query.exec()) {
        if (error != nullptr) *error = query.lastError().text();
        return false;
    }
    if (!query.next()) return true;
    query.finish();
    query.prepare(key.isEmpty() ? QStringLiteral("SELECT MAX(version) FROM %1").arg(table)
                                : QStringLiteral("SELECT value FROM %1 WHERE key = ?").arg(table));
    if (!key.isEmpty()) query.addBindValue(key);
    if (!query.exec()) {
        if (error != nullptr) *error = query.lastError().text();
        return false;
    }
    if (!query.next() || query.value(0).isNull()) return true;
    bool ok = false;
    const qint64 version = query.value(0).toLongLong(&ok);
    if (!ok || version < 0 || version > supported) {
        if (error != nullptr) *error = !ok || version < 0 ? QStringLiteral("Invalid schema version metadata")
            : QStringLiteral("Unsupported schema version %1 (maximum supported: %2)").arg(version).arg(supported);
        return false;
    }
    return true;
}

QString likeEscaped(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return value;
}

bool tableHasColumn(QSqlDatabase database, const QString &table, const QString &column)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == column) {
            return true;
        }
    }
    return false;
}

bool ensureColumn(QSqlDatabase database, const QString &table, const QString &column,
                  const QString &definition, QString *error)
{
    if (tableHasColumn(database, table, column)) {
        return true;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2").arg(table, definition))) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

QString sqlPlaceholders(qsizetype count)
{
    QStringList marks;
    marks.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        marks << QStringLiteral("?");
    }
    return marks.join(QStringLiteral(", "));
}

} // namespace SqlUtil
