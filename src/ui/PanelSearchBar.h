#pragma once

#include "search/SearchMatcher.h"

#include <QKeySequence>
#include <QList>
#include <QWidget>
#include <QVector>

#include <functional>

class QLabel;
class QLineEdit;
class QKeyEvent;

// Reusable ncmpcpp-style "/" search bar for a single list/tree/table view.
//
// Behavior (matches the main view's panel search):
//   /            open the bar, focus the edit
//   <type>       live-filter; the cursor jumps to the first match
//   Return       confirm — hide the bar but KEEP the query/matches resident, so
//                M-n / M-p keep cycling with the list focused (if there were no
//                matches the query is dropped instead)
//   M-n / M-p    cycle to the next / previous match (wraps); opens the bar first
//                if there is no active query
//   Esc          first clears a non-empty query, then dismisses the bar
//   C-f          toggle fuzzy matching
//
// The host owns row navigation and supplies it through Providers; the bar owns
// the query/match state and the edit-key handling. Bind the host's Search /
// SearchNext / SearchPrevious / Escape actions to open()/cycle()/escape(), and
// pass the same actions' keys to setCommandKeys() so the edit agrees with them.
class PanelSearchBar final : public QWidget {
    Q_OBJECT

public:
    struct Providers {
        // Searchable documents for the current rows, in display order.
        std::function<QVector<Search::MatchDocument>()> documents;
        std::function<int()> rowCount;
        std::function<int()> currentRow;
        std::function<void(int row)> setCurrentRow;
        // Return keyboard focus to the host list after confirm/escape.
        std::function<void()> focusList;
    };

    explicit PanelSearchBar(QWidget *parent = nullptr);

    void setProviders(Providers providers);
    // Display name of the searched view, e.g. "Queue" -> prompt "Search Queue:".
    void setLabel(const QString &label);
    // The edit's keys for escape() and cycle(); Esc, M-n and M-p until set.
    // Keys that type text are left to the edit.
    void setCommandKeys(const QList<QKeySequence> &escapeKeys, const QList<QKeySequence> &nextKeys,
                        const QList<QKeySequence> &previousKeys);

    void open();
    void cycle(int direction); // +1 next, -1 previous
    // Returns true if the Escape was consumed (the bar was visible / had a query).
    bool escape();
    // Recompute matches/status against the current rows without moving the
    // cursor — call when the underlying list changes while a search is open.
    void refresh();

    bool isSearchVisible() const;
    bool hasActiveQuery() const;

signals:
    void statusMessage(const QString &message, int timeoutMs);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool handleEditKey(QKeyEvent *event);
    void confirm();
    void setQuery(const QString &query);
    void rebuildMatches(bool jumpToFirst);
    void updateUi();

    Providers m_providers;
    QString m_label = QStringLiteral("Panel");
    QString m_query;
    QVector<Search::PanelMatch> m_matches;
    int m_currentMatch = -1;
    bool m_fuzzy = false;
    QList<QKeySequence> m_escapeKeys{QKeySequence(Qt::Key_Escape)};
    QList<QKeySequence> m_nextKeys{QKeySequence(Qt::ALT | Qt::Key_N)};
    QList<QKeySequence> m_previousKeys{QKeySequence(Qt::ALT | Qt::Key_P)};

    QLabel *m_prompt = nullptr;
    QLineEdit *m_edit = nullptr;
    QLabel *m_status = nullptr;
};
