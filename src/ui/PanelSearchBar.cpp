#include "ui/PanelSearchBar.h"

#include "search/SearchQuery.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>

PanelSearchBar::PanelSearchBar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    m_prompt = new QLabel(this);
    m_edit = new QLineEdit(this);
    m_edit->setClearButtonEnabled(true);
    m_status = new QLabel(this);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    layout->addWidget(m_prompt);
    layout->addWidget(m_edit, 1);
    layout->addWidget(m_status);

    setVisible(false);
    m_edit->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &text) {
        setQuery(text);
    });
    updateUi();
}

void PanelSearchBar::setProviders(Providers providers)
{
    m_providers = std::move(providers);
    updateUi();
}

void PanelSearchBar::setLabel(const QString &label)
{
    m_label = label;
    updateUi();
}

bool PanelSearchBar::isSearchVisible() const
{
    return isVisible();
}

bool PanelSearchBar::hasActiveQuery() const
{
    return !m_query.trimmed().isEmpty();
}

void PanelSearchBar::open()
{
    setVisible(true);
    updateUi();
    const QSignalBlocker blocker(m_edit);
    m_edit->setText(m_query);
    m_edit->setFocus();
    m_edit->selectAll();
}

bool PanelSearchBar::escape()
{
    // First Escape clears a non-empty query (textChanged rebuilds); a second one
    // drops the query/matches entirely and dismisses the bar.
    if (!m_edit->text().isEmpty()) {
        m_edit->clear();
        return true;
    }
    const bool wasActive = isVisible() || hasActiveQuery();
    m_query.clear();
    m_matches.clear();
    m_currentMatch = -1;
    setVisible(false);
    if (m_providers.focusList) {
        m_providers.focusList();
    }
    return wasActive;
}

void PanelSearchBar::confirm()
{
    // ncmpcpp-style confirm: with matches, keep the query and current match so
    // M-n / M-p keep cycling after the bar closes; with none, drop the query so
    // the view closes free of any constraint.
    if (m_matches.isEmpty()) {
        m_query.clear();
        m_currentMatch = -1;
    }
    setVisible(false);
    if (m_providers.focusList) {
        m_providers.focusList();
    }
    updateUi();
}

void PanelSearchBar::setQuery(const QString &query)
{
    // Keep the raw text (don't trim) so spaces survive while typing; the parser
    // skips empty whitespace tokens.
    m_query = query;
    rebuildMatches(/*jumpToFirst=*/true);
    updateUi();
}

void PanelSearchBar::rebuildMatches(bool jumpToFirst)
{
    m_matches.clear();
    m_currentMatch = -1;

    if (m_query.trimmed().isEmpty() || !m_providers.documents) {
        updateUi();
        return;
    }

    m_matches = Search::matchDocumentsInDisplayOrder(
        m_providers.documents(), Search::SearchQuery::parse(m_query), m_fuzzy);

    if (!m_matches.isEmpty()) {
        int matchIndex = 0;
        const int current = m_providers.currentRow ? m_providers.currentRow() : 0;
        for (int i = 0; i < m_matches.size(); ++i) {
            if (m_matches.at(i).row >= current) {
                matchIndex = i;
                break;
            }
        }
        m_currentMatch = matchIndex;
        if (jumpToFirst && m_providers.setCurrentRow) {
            m_providers.setCurrentRow(m_matches.at(matchIndex).row);
        }
    } else {
        emit statusMessage(QStringLiteral("No matches for '%1' in %2").arg(m_query.trimmed(), m_label), 3000);
    }
    updateUi();
}

void PanelSearchBar::cycle(int direction)
{
    if (!hasActiveQuery()) {
        open();
        return;
    }
    rebuildMatches(/*jumpToFirst=*/false);
    if (m_matches.isEmpty()) {
        return;
    }
    if (m_currentMatch < 0) {
        m_currentMatch = 0;
    } else {
        const qsizetype size = m_matches.size();
        const qsizetype next = (static_cast<qsizetype>(m_currentMatch) + direction + size) % size;
        m_currentMatch = static_cast<int>(next);
    }
    if (m_providers.setCurrentRow) {
        m_providers.setCurrentRow(m_matches.at(m_currentMatch).row);
    }
    updateUi();
}

void PanelSearchBar::updateUi()
{
    m_prompt->setText(QStringLiteral("Search %1:").arg(m_label));

    const QString mode = m_fuzzy ? QStringLiteral("fuzzy") : QStringLiteral("exact");
    const int rows = m_providers.rowCount ? m_providers.rowCount() : 0;
    if (rows == 0) {
        m_status->setText(QStringLiteral("No rows · %1").arg(mode));
    } else if (m_query.trimmed().isEmpty()) {
        m_status->setText(QStringLiteral("%1 rows · %2").arg(rows).arg(mode));
    } else if (m_matches.isEmpty()) {
        m_status->setText(QStringLiteral("No matches · %1").arg(mode));
    } else {
        m_status->setText(QStringLiteral("%1/%2 · %3")
                              .arg(m_currentMatch + 1)
                              .arg(m_matches.size())
                              .arg(mode));
    }
}

bool PanelSearchBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        return handleEditKey(static_cast<QKeyEvent *>(event));
    }
    return QWidget::eventFilter(watched, event);
}

bool PanelSearchBar::handleEditKey(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool hasCtrlAltMeta =
        (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) != 0;

    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !hasCtrlAltMeta) {
        confirm();
        return true;
    }
    if (event->key() == Qt::Key_Escape) {
        escape();
        return true;
    }
    if (mods.testFlag(Qt::AltModifier) && event->key() == Qt::Key_N) {
        cycle(+1);
        return true;
    }
    if (mods.testFlag(Qt::AltModifier) && event->key() == Qt::Key_P) {
        cycle(-1);
        return true;
    }
    if (mods == Qt::ControlModifier && event->key() == Qt::Key_F) {
        m_fuzzy = !m_fuzzy;
        rebuildMatches(/*jumpToFirst=*/false);
        return true;
    }
    return false;
}
