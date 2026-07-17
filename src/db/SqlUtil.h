#pragma once

#include <QSqlDatabase>
#include <QString>

namespace SqlUtil {

QString likeEscaped(QString value);
bool tableHasColumn(QSqlDatabase database, const QString &table, const QString &column);
bool ensureColumn(QSqlDatabase database, const QString &table, const QString &column,
                  const QString &definition, QString *error);
QString sqlPlaceholders(qsizetype count);

} // namespace SqlUtil
