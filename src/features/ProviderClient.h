#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace FeatureProvider {

struct Resolved {
    QString path;
    QString source;
    QJsonObject capabilities;
};

struct Invocation {
    int exitCode = 4;
    QJsonObject result;
    QString errorCode;
    QString errorMessage;
};

std::optional<Resolved> discover(const QString &explicitPath, const QString &savedPath);

Invocation invoke(const QString &path,
                  const QString &operation,
                  const QJsonObject &parameters,
                  bool relayJsonl,
                  const std::function<bool()> &canceled,
                  int operationTimeoutMs = 5'000);

} // namespace FeatureProvider
