#include "db/SqlUtil.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace SqlUtil {

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
