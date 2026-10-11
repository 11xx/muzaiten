#pragma once

#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>

namespace FingerprintCursorFixture {

// A connection-local view preserves the table's columns and COUNT queries.
// Reading file_mtime on the last row raises a SQLite integer overflow.
inline QString install(const QSqlDatabase &connection)
{
    const QSqlRecord record = connection.record(QStringLiteral("tracks"));
    QStringList columns;
    for (int i = 0; i < record.count(); ++i) {
        const QString column = record.fieldName(i);
        if (column == QLatin1String("file_mtime")) {
            columns.append(QStringLiteral(
                "CASE WHEN id = (SELECT MAX(id) FROM main.tracks) "
                "THEN abs(-9223372036854775808) ELSE file_mtime END AS file_mtime"));
        } else {
            columns.append(connection.driver()->escapeIdentifier(column, QSqlDriver::FieldName));
        }
    }
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral("CREATE TEMP VIEW tracks AS SELECT %1 FROM main.tracks ORDER BY id")
                        .arg(columns.join(QLatin1Char(','))))) {
        return query.lastError().text();
    }
    return {};
}

} // namespace FingerprintCursorFixture
