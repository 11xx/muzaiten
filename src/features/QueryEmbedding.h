#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

// Text -> CLAP query vector through the muzaiten-features orchestrator.
// Shared by muzaitenctl and the GUI so both speak the same discovery,
// timeout, and error language. Blocking: run it off the UI thread.
namespace QueryEmbedding {

struct Result {
    QVector<float> vector;
    QJsonObject metadata; // provider result: model identity + revision
    QString error;        // empty on success

    bool ok() const { return !vector.isEmpty(); }
};

// Parses a raw JSON vector payload: bare array, or an object carrying
// "vector"/"embedding". Used for --query-vector-json style inputs too.
QVector<float> parseVectorJson(const QByteArray &json, QString *error);

// Spawns `muzaiten-features query <text> --json` (sibling binary first,
// then PATH) and parses its terminal JSON.
Result viaFeatures(const QString &text, int timeoutMs);

} // namespace QueryEmbedding
