#include "features/QueryEmbedding.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

#include <cmath>

namespace QueryEmbedding {
namespace {

QString compactProcessError(QString value)
{
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    if (value.size() > 500) {
        value = value.left(500).trimmed() + QStringLiteral("...");
    }
    return value;
}

QVector<float> normalizedVector(const QVector<float> &values, QString *error)
{
    if (values.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("embedding vector is empty");
        }
        return {};
    }

    double normSquared = 0.0;
    for (float value : values) {
        if (!std::isfinite(value)) {
            if (error != nullptr) {
                *error = QStringLiteral("embedding vector contains a non-finite value");
            }
            return {};
        }
        normSquared += static_cast<double>(value) * static_cast<double>(value);
    }
    if (!(normSquared > 0.0) || !std::isfinite(normSquared)) {
        if (error != nullptr) {
            *error = QStringLiteral("embedding vector must have a finite non-zero norm");
        }
        return {};
    }

    const double scale = 1.0 / std::sqrt(normSquared);
    QVector<float> normalized;
    normalized.reserve(values.size());
    for (float value : values) {
        normalized.push_back(static_cast<float>(static_cast<double>(value) * scale));
    }
    return normalized;
}

QVector<float> vectorFromJsonArray(const QJsonArray &array, QString *error)
{
    QVector<float> raw;
    raw.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QJsonValue value = array.at(i);
        if (!value.isDouble()) {
            if (error != nullptr) {
                *error = QStringLiteral("embedding vector entry %1 is not numeric").arg(i);
            }
            return {};
        }
        const double number = value.toDouble();
        if (!std::isfinite(number)) {
            if (error != nullptr) {
                *error = QStringLiteral("embedding vector entry %1 is not finite").arg(i);
            }
            return {};
        }
        raw.push_back(static_cast<float>(number));
    }
    return normalizedVector(raw, error);
}

} // namespace

QVector<float> parseVectorJson(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("could not parse query embedding JSON: %1").arg(parseError.errorString());
        }
        return {};
    }

    if (document.isArray()) {
        return vectorFromJsonArray(document.array(), error);
    }
    if (!document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("query embedding JSON must be an array or object");
        }
        return {};
    }

    const QJsonObject object = document.object();
    const QJsonValue vectorValue = object.value(QStringLiteral("vector")).isArray()
        ? object.value(QStringLiteral("vector"))
        : object.value(QStringLiteral("embedding"));
    if (!vectorValue.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("query embedding JSON object must contain a vector array");
        }
        return {};
    }
    return vectorFromJsonArray(vectorValue.toArray(), error);
}

Result viaFeatures(const QString &text, int timeoutMs)
{
    Result result;
    const QString sibling = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("muzaiten-features"));
    const QString executable = QFileInfo(sibling).isExecutable()
        ? sibling
        : QStandardPaths::findExecutable(QStringLiteral("muzaiten-features"));
    if (executable.isEmpty()) {
        result.error = QStringLiteral("semantic search requires muzaiten-features");
        return result;
    }

    QProcess process;
    process.start(executable, {QStringLiteral("query"), text, QStringLiteral("--json")});
    if (!process.waitForStarted(5000)) {
        result.error = QStringLiteral("semantic search could not start muzaiten-features");
        return result;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        result.error = QStringLiteral("semantic search query through muzaiten-features timed out");
        return result;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (detail.isEmpty()) {
            detail = QStringLiteral("query failed");
        }
        result.error = QStringLiteral("semantic search requires a ready CLAP provider: %1")
                           .arg(compactProcessError(detail));
        return result;
    }

    QString parseError;
    const QByteArray output = process.readAllStandardOutput();
    const QJsonDocument document = QJsonDocument::fromJson(output);
    if (document.isObject()) {
        result.metadata = document.object();
    }
    result.vector = parseVectorJson(output, &parseError);
    if (result.vector.isEmpty()) {
        result.error = QStringLiteral("semantic search requires a ready CLAP provider: %1").arg(parseError);
    }
    return result;
}

} // namespace QueryEmbedding
