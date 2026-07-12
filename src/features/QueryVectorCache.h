#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

// Persistent text -> query-vector memo for semantic search. A query vector
// depends only on the provider model identity (checkpoint sha + feature
// revision), never on library contents, so a hit skips the provider process
// and its ~1 s cold ONNX session load entirely. Lives under the cache dir:
// disposable, recomputable, safe to delete at any time.
class QueryVectorCache
{
public:
    struct Identity {
        QString capability;
        QString model;
        QString checkpointSha256;
        QString featureRevision;
        int vectorDimension = 0;

        bool valid() const
        {
            return !capability.isEmpty() && !model.isEmpty() && !checkpointSha256.isEmpty()
                && !featureRevision.isEmpty() && vectorDimension > 0;
        }
    };

    explicit QueryVectorCache(const QString &databasePath);
    ~QueryVectorCache();

    QueryVectorCache(const QueryVectorCache &) = delete;
    QueryVectorCache &operator=(const QueryVectorCache &) = delete;

    bool isOpen() const;
    QVector<float> lookup(const Identity &identity, const QString &queryText);
    bool store(const Identity &identity, const QString &queryText, const QVector<float> &vector);

    // XDG cache location shared by muzaitenctl and the GUI.
    static QString defaultPath();
    // Whitespace-insensitive key form so "piano  jazz" and "piano jazz" hit
    // the same row; semantic content is unchanged by whitespace runs.
    static QString normalizedQueryText(const QString &text);

private:
    bool ensureSchema();
    void pruneIfOversized();

    QString m_connectionName;
    QSqlDatabase m_db;
    bool m_connectionAdded = false;
    bool m_open = false;
};
