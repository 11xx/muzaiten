#pragma once

#include "core/Track.h"
#include "search/RankConfig.h"
#include "search/ResultRanker.h"
#include "search/SearchIndex.h"

#include <QDialog>
#include <QSet>
#include <QString>

class QLabel;
class QLineEdit;
class QListView;
class QThread;
class QTimer;
class SearchResultsModel;
class SearchResultDelegate;

namespace Search {
class SearchWorker;
}

// Add-song modal for the playlist view. Reuses the search engine (its own
// SearchWorker + index) and the search results model/delegate, but RET adds the
// highlighted track to the playlist and readies the box for the next one instead
// of playing. Already-added tracks are washed so duplicates are obvious.
//
// Interaction:
//   type            incremental search (album-grouped, then quality ranking)
//   Down/C-n Up/C-p move the cursor
//   RET             add current track, clear box, ready for the next
//   C-/             undo the last add (restores its query to the box)
//   C-g / Esc       clear the box; on an empty box, close the modal
class PlaylistAddDialog final : public QDialog {
    Q_OBJECT
public:
    PlaylistAddDialog(const QString &dbPath, const QString &playlistName, QWidget *parent = nullptr);
    ~PlaylistAddDialog() override;

    void setAddedPaths(const QSet<QString> &paths);
    // Pre-fills the query box (edit mode / undo restore) and selects the text.
    void setQueryText(const QString &query);
    // In edit mode the dialog chooses a single replacement and closes.
    void setEditMode(bool editMode) { m_editMode = editMode; }
    // A one-line reference shown above the search box in edit mode: what the row
    // being edited expects (e.g. "Imported as: …", "Editing: …"). Empty hides it.
    void setEditContext(const QString &text);
    // Stored MultiMatch candidates: hoisted to the top of every result list so
    // the import's shortlist is one keypress away.
    void setPreferredPaths(const QStringList &paths);

signals:
    // Emitted on RET with the chosen track and the query that produced it.
    void itemChosen(const Track &track, const QString &query);
    void undoRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void onDebounceTimeout();
    void onIndexReady(int count);
    void onIndexError(const QString &error);
    void onResultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int totalMatches);

private:
    void submitQuery();
    void moveCursor(int delta);
    void setCursorRow(int row);
    void chooseCurrent();
    void updateStatus();

    QLabel               *m_editHeader = nullptr;
    QLineEdit            *m_box      = nullptr;
    QLabel               *m_status   = nullptr;
    QListView            *m_list     = nullptr;
    SearchResultsModel   *m_model    = nullptr;
    SearchResultDelegate *m_delegate = nullptr;
    QTimer               *m_debounce = nullptr;

    QThread              *m_workerThread = nullptr;
    Search::SearchWorker *m_worker       = nullptr;

    Search::RankConfig   m_rankConfig;
    Search::ResultRanker m_ranker;

    QString m_playlistName;
    QSet<QString> m_preferredPaths;
    bool    m_indexLoaded = false;
    bool    m_editMode    = false;
    quint64 m_queryId     = 0;
    int     m_totalIndexed = 0;
    int     m_matchCount   = 0;
};
