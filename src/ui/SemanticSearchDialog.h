#pragma once

#include "core/Track.h"
#include "ui/SemanticSearchData.h"

#include <QDialog>
#include <QHash>
#include <QVector>

#include <atomic>
#include <memory>
#include <thread>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Describe-the-music search over the CLAP embeddings (Ctrl+S). One shot per
// Enter: the query text becomes a vector (query cache first, provider
// process on a miss), every embedded group is cosine-ranked, and the top
// matches surface as rich rows the queue actions understand. All storage
// work runs on a worker thread against its own connections; the embeddings
// matrix loads once per dialog and is reused across queries.
class SemanticSearchDialog final : public QDialog {
    Q_OBJECT

public:
    SemanticSearchDialog(QString libraryDbPath, QString featuresDbPath, QWidget *parent = nullptr);
    ~SemanticSearchDialog() override;

    void focusQuery();

signals:
    void addToQueueRequested(QVector<Track> tracks);
    void playNextRequested(QVector<Track> tracks);
    void playNowRequested(QVector<Track> tracks);
    void statusMessageRequested(QString message, int timeoutMs);

private:
    struct ResultRow {
        Track track;
        qint64 groupId = 0;
        double score = 0.0;
    };
    struct QueryOutcome {
        QVector<ResultRow> rows;
        QString error;
        QString statusDetail;
        std::shared_ptr<QHash<qint64, QVector<float>>> embeddings; // cache handoff
    };

    void submitQuery();
    void presentOutcome(quint64 generation, QueryOutcome outcome);
    void emitForSelection(void (SemanticSearchDialog::*signal)(QVector<Track>));
    QVector<Track> selectedTracks() const;
    void joinWorker();

    QString m_libraryDbPath;
    QString m_featuresDbPath;
    QLineEdit *m_query = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_results = nullptr;
    QPushButton *m_playNow = nullptr;
    QPushButton *m_playNext = nullptr;
    QPushButton *m_enqueue = nullptr;
    QVector<ResultRow> m_rows;
    // Loaded by the first query's worker, reused read-only afterwards.
    std::shared_ptr<QHash<qint64, QVector<float>>> m_embeddings;
    std::thread m_worker;
    std::atomic<quint64> m_generation{0};
};
