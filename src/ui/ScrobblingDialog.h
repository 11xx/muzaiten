#pragma once

#include <QDialog>

class ListeningHistoryPanel;
class QPushButton;
class QTabWidget;
class ScrobblersPanel;

// The one window for scrobbling: which destinations exist and how they are
// configured, and what has actually been delivered to them. They are two views
// of the same subject and are read against each other constantly (a backlog in
// one is explained by a token or an address in the other), so they are tabs of
// one window rather than two windows reached from two menu entries.
//
// Nothing here is accepted or cancelled. Both panels save as they are edited,
// and take effect with it.
class ScrobblingDialog final : public QDialog {
    Q_OBJECT

public:
    enum class Tab {
        Scrobblers,
        History,
    };

    // Takes ownership of both panels.
    ScrobblingDialog(ScrobblersPanel *scrobblers, ListeningHistoryPanel *history, QWidget *parent = nullptr);

    void showTab(Tab tab);


private:
    QTabWidget *m_tabs = nullptr;
    QPushButton *m_close = nullptr;
};
