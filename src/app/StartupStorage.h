#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <stdexcept>

namespace StartupStorage {

struct Issue {
    QString store;
    QString path;
    QString error;
    bool required = true;
};

struct Report {
    QVector<Issue> issues;
    QString phase = QStringLiteral("preflight");
    bool ok() const;
    QJsonObject json() const;
    QString summary() const;
};

class Failure final : public std::runtime_error {
public:
    explicit Failure(Report report);
    const Report report;
};

Report inspect();
[[noreturn]] void fail(const QString &store, const QString &path, const QString &error);

} // namespace StartupStorage
