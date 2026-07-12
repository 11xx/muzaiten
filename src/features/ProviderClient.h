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

// First candidate that exists and is executable, WITHOUT the capabilities
// handshake (capabilities left empty). Interactive one-shot operations use
// this to avoid paying a full provider process just to probe; callers must
// fall back to discover() when the trusted invocation fails.
std::optional<Resolved> resolveTrusted(const QString &explicitPath, const QString &savedPath);

Invocation invoke(const QString &path,
                  const QString &operation,
                  const QJsonObject &parameters,
                  bool relayJsonl,
                  const std::function<bool()> &canceled,
                  int operationTimeoutMs = 5'000);

} // namespace FeatureProvider
