#pragma once

#include <QSqlDatabase>
#include <QString>

namespace SqlUtil {

QString likeEscaped(QString value);
bool tableHasColumn(QSqlDatabase database, const QString &table, const QString &column);
bool ensureColumn(QSqlDatabase database, const QString &table, const QString &column,
                  const QString &definition, QString *error);
QString sqlPlaceholders(qsizetype count);

class Savepoint final {
public:
    Savepoint(QSqlDatabase database, QString *error);
    ~Savepoint();
    Savepoint(const Savepoint &) = delete;
    Savepoint &operator=(const Savepoint &) = delete;
    bool active() const { return m_active; }
    bool commit();
private:
    QSqlDatabase m_database;
    QString m_name;
    QString *m_error;
    bool m_active = false;
};

bool validateSchemaVersion(QSqlDatabase database, const QString &table,
                           const QString &key, int supported, QString *error);

} // namespace SqlUtil
