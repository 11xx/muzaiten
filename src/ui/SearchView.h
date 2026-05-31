#pragma once

#include "search/SearchIndex.h"
#include "search/SearchRecord.h"

#include <QWidget>
#include <QVector>

class QLabel;
class QLineEdit;
class QListView;
class QThread;
class QTimer;
class SearchResultDelegate;
class SearchResultsModel;

namespace Search {
class SearchWorker;
}

struct Track;

class SearchView : public QWidget {
    Q_OBJECT
public:
    explicit SearchView(QWidget *parent = nullptr);
    ~SearchView() override;

    // Called by MainWindow when the view becomes visible for the first time.
    void ensureIndexLoaded(const QString &dbPath);
    // Rebuild the index (called after a scan / MPD import finishes).
    void invalidateIndex(const QString &dbPath);

    // Give focus to the search box.
    void focusSearchBox();

signals:
    void addToQueueRequested(QVector<Track> tracks);
    void playNowRequested(QVector<Track> tracks);
    void leaveRequested();

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTextChanged();
    void onDebounceTimeout();
    void onIndexReady(int count);
    void onIndexError(const QString &error);
    void onResultsReady(quint64 queryId, QVector<Search::ScoredResult> results);

private:
    void setupUi();
    void setupWorker(const QString &dbPath);
    void teardownWorker();
    void submitQuery();
    void updateStatusLabel();
    QVector<Track> selectedTracks() const;
    void activateSelection(bool playNow);
    void toggleFuzzyMode();
    void adjustRowHeight(int delta);

    QLineEdit            *m_searchBox    = nullptr;
    QLabel               *m_statusLabel  = nullptr;
    QListView            *m_resultList   = nullptr;
    SearchResultsModel   *m_model        = nullptr;
    SearchResultDelegate *m_delegate     = nullptr;
    QTimer               *m_debounce     = nullptr;

    QThread              *m_workerThread = nullptr;
    Search::SearchWorker *m_worker       = nullptr;

    QString   m_dbPath;
    bool      m_indexLoaded  = false;
    bool      m_fuzzyMode    = false;
    quint64   m_queryId      = 0;
    int       m_totalIndexed = 0;
    int       m_rowHeight    = 0;  // 0 = delegate auto
};
