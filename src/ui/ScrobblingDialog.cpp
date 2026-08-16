#include "ui/ScrobblingDialog.h"

#include "ui/ListeningHistoryPanel.h"
#include "ui/ScrobblersPanel.h"

#include <QDialogButtonBox>
#include <QTabWidget>
#include <QVBoxLayout>

ScrobblingDialog::ScrobblingDialog(ScrobblersPanel *scrobblers, ListeningHistoryPanel *history, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Scrobbling"));
    resize(1100, 650);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(scrobblers, QStringLiteral("Scrobblers"));
    m_tabs->addTab(history, QStringLiteral("Listening history"));

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
