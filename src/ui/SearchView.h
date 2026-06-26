#pragma once

#include "core/Track.h"
#include "search/RankConfig.h"
#include "search/ResultRanker.h"
#include "search/SearchIndex.h"
#include "search/SearchRecord.h"
#include "ui/KeyBindingTypes.h"

#include <QWidget>
#include <QVector>

class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QThread;
class QTimer;
class SearchResultDelegate;
class SearchResultsModel;

namespace Search {
class SearchWorker;
}

class SearchView : public QWidget {
    Q_OBJECT
public:
    explicit SearchView(QWidget *parent = nullptr);
    ~SearchView() override;

    // Called by MainWindow when the view becomes visible; builds the in-memory
    // index from the database on first use (kept resident for the session, with
    // a delayed release after the view is hidden — see the cleanup timer).
    void ensureIndexLoaded(const QString &dbPath);
    // Force a rebuild of the index (after a scan/import, or manual F5 / re-press 3).
    void invalidateIndex(const QString &dbPath);
    void forceRefresh();

    // Give keyboard focus to the search box ("input mode").
    void focusSearchBox();

    // Apply a new ranking/exclusion config: rebuilds the front-end ranker,
    // forwards exclusions to the worker, and re-runs the current query.
    void setRankConfig(const Search::RankConfig &config);

    // Key/action reference for the Keybinds dialog. Kept next to handleNavKey()
    // in the .cpp — update both together.
    static KeyBindingReferenceList keyBindingReference();

    // When true, the context menu also offers "(don't save to playlist)" queue
    // adds (the queue is mirroring a playlist).
    void setQueueIsPlaylistSourced(bool sourced) { m_queueIsPlaylistSourced = sourced; }

signals:
    void addToQueueRequested(QVector<Track> tracks);
    void addToPlaylistRequested(QVector<Track> tracks);
    void playNextRequested(QVector<Track> tracks);
    // "(don't save to playlist)" variants: add to the queue only, never mirroring
    // into the playlist that backs the queue.
    void addToQueueTemporaryRequested(QVector<Track> tracks);
    void playNextTemporaryRequested(QVector<Track> tracks);
    void playNowRequested(QVector<Track> tracks);
    void findInLibraryRequested(Track track);
    void findFileRequested(Track track);
    void propertiesRequested(Track track);

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTextChanged();
    void onDebounceTimeout();
    void onIndexGrew(int count);
    void onIndexLoaded(int count);
    void onIndexRefreshing();
    void onIndexRefreshed(int count);
    void onIndexError(const QString &error);
    void onResultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int totalMatches);
    void onSpinnerTick();
    void showContextMenu(const QPoint &pos);
    void onDoubleClicked(const QModelIndex &index);

private:
    void setupUi();
    void setupWorker(const QString &dbPath);
    void teardownWorker();
    void submitQuery();
    void updateStatusLabel();
    // Frees the resident search index when idle-hidden; the index is rebuilt on
    // the next show (and on explicit navigation back to search).
    void releaseIdleResources();

    // Returns true if the key was a navigation/action key we handled.
    bool handleNavKey(class QKeyEvent *ke);
    void moveCursor(int delta);
    void setCursorRow(int row, bool select = false);
    void toggleMarkAtCursor();
    void escapeOrClear();
    void releaseInputFocus();

    QVector<Track> selectedTracks() const;
    QVector<Track> tracksForAction(const QModelIndex &clicked) const;
    Track trackAt(const QModelIndex &index) const;
    void activateSelection(bool playNow);
    void toggleFuzzyMode();
    void adjustRowHeight(int delta);

    QLineEdit            *m_searchBox    = nullptr;
    QLabel               *m_statusLabel  = nullptr;
    QListView            *m_resultList   = nullptr;
    SearchResultsModel   *m_model        = nullptr;
    SearchResultDelegate *m_delegate     = nullptr;
    QTimer               *m_debounce     = nullptr;
    QTimer               *m_spinnerTimer = nullptr;
    QTimer               *m_streamRerunTimer = nullptr;
    QTimer               *m_cacheMsgTimer = nullptr;

    QThread              *m_workerThread = nullptr;
    Search::SearchWorker *m_worker       = nullptr;

    Search::RankConfig   m_rankConfig;
    Search::ResultRanker m_ranker;

    QString   m_dbPath;
    bool      m_indexLoaded  = false;  // first batch arrived (or load done) — queries enabled
    bool      m_indexStreaming = false; // a cold streaming build is in progress (main spinner)
    bool      m_indexRefreshing = false; // a background cache-refresh is in progress (secondary spinner)
    bool      m_cacheUpdatedRecently = false; // flash "cache updated" after a refresh
    int       m_spinnerFrame = 0;
    bool      m_fuzzyMode    = false;
    quint64   m_queryId      = 0;
    int       m_totalIndexed = 0;
    int       m_matchCount   = 0;  // total matches for the current query (uncapped)
    int       m_rowHeight    = 0;  // 0 = delegate auto
    bool      m_queueIsPlaylistSourced = false;
};
