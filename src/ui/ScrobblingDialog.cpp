#include "ui/ScrobblingDialog.h"

#include "ui/ListeningHistoryPanel.h"
#include "ui/ScrobblersPanel.h"

#include <QDialogButtonBox>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

ScrobblingDialog::ScrobblingDialog(ScrobblersPanel *scrobblers, ListeningHistoryPanel *history, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Scrobbling"));
    resize(1100, 650);

    m_tabs = new QTabWidget(this);
    // A thin, square label strip rather than the theme's rounded cards: with one
    // pane below two tabs, the card shape spends height on decoration and reads
    // as a container the panels are not in. Every colour is still the palette's,
    // so the strip follows the active theme even though it is not drawn by it.
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setStyleSheet(QStringLiteral(
        "QTabBar::tab{border:none;border-radius:0;padding:4px 14px;margin:0;"
        "background:transparent;color:palette(window-text);}"
        "QTabBar::tab:hover{background:palette(alternate-base);}"
        "QTabBar::tab:selected{border-bottom:2px solid palette(highlight);"
        "color:palette(window-text);}"
        // Taking over the tab's shape takes over its focus ring too, and a
        // keyboard user needs to see which strip has the focus.
        "QTabBar::tab:focus{border:1px dotted palette(window-text);"
        "border-bottom:2px solid palette(highlight);}"));
    m_tabs->addTab(scrobblers, QStringLiteral("Scrobblers"));
    m_tabs->addTab(history, QStringLiteral("Listening history"));

    // Editing a destination in one tab changes what the other is looking at.
    // Carrying that across is the reason these two are one window.
    connect(scrobblers, &ScrobblersPanel::destinationsChanged, history, &ListeningHistoryPanel::setDestinations);
    // And queueing, retrying or clearing a backlog changes the pending count the
    // other tab is showing for that destination.
    connect(history, &ListeningHistoryPanel::backlogChanged, scrobblers,
            [scrobblers]() { scrobblers->refreshPendingCounts(); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(buttons);
}

void ScrobblingDialog::showTab(Tab tab)
{
    m_tabs->setCurrentIndex(tab == Tab::Scrobblers ? 0 : 1);
}
