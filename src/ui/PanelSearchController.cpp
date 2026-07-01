#include "ui/PanelSearchController.h"

#include "search/SearchQuery.h"
#include "ui/NavigableTableView.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>

namespace {

QKeySequence keySequenceForEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(static_cast<int>(mods) | event->key());
}

} // namespace

PanelSearchController::PanelSearchController(QWidget *parent)
    : QWidget(parent)
    , m_focusOrder(defaultMainPanelFocusOrder())
    , m_keyBindings(mainPanelBindingMapForProfile(defaultMainPanelKeyBindingProfileName()))
    , m_keyBindingProfileName(defaultMainPanelKeyBindingProfileName())
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    m_prompt = new QLabel(this);
    m_edit = new QLineEdit(this);
    m_status = new QLabel(this);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_edit->setClearButtonEnabled(true);

    layout->addWidget(m_prompt);
    layout->addWidget(m_edit, 1);
    layout->addWidget(m_status);

    m_edit->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &text) {
        setQueryForActivePanel(text);
    });

    setVisible(false);
    updateSearchUi();
}

void PanelSearchController::registerTarget(MainPanelTarget target)
{
    if (target.focusWidget == nullptr) {
        return;
    }
    installTargetMappings(target);
    if (!m_state.contains(target.id)) {
        m_state.insert(target.id, {});
    }
    m_targets.push_back(std::move(target));
    updatePanelActiveProperties();
}

void PanelSearchController::replaceTarget(MainPanelTarget target)
{
    if (target.focusWidget == nullptr) {
        return;
    }

    for (MainPanelTarget &registered : m_targets) {
        if (registered.id != target.id) {
            continue;
        }
        removeTargetMappings(registered);
        registered = std::move(target);
        installTargetMappings(registered);
        if (!m_state.contains(registered.id)) {
            m_state.insert(registered.id, {});
        }
        updatePanelActiveProperties();
        updateSearchUi();
        return;
    }

    registerTarget(std::move(target));
}

void PanelSearchController::setKeyBindingProfileName(const QString &name)
{
    m_keyBindingProfileName = name.isEmpty() ? defaultMainPanelKeyBindingProfileName() : name;
    m_keyBindings = mainPanelBindingMapForProfile(m_keyBindingProfileName);
}

void PanelSearchController::setFocusOrder(const QVector<MainPanelId> &order)
{
    m_focusOrder = order.isEmpty() ? defaultMainPanelFocusOrder() : order;
    // If the active panel was removed from the order, snap to the first one so
    // h/l navigation (which indexes the active panel into m_focusOrder) keeps
    // working.
    if (m_hasActivePanel && !m_focusOrder.contains(m_activePanel)) {
        setActivePanel(m_focusOrder.first(), false);
    }
}

void PanelSearchController::activateForMainView()
{
    m_mainViewActive = true;
    if (!m_hasActivePanel) {
        setActivePanel(MainPanelId::Artists, false);
    }
    updatePanelActiveProperties();
    updateSearchUi();
    QTimer::singleShot(0, this, [this]() {
        if (m_mainViewActive && !m_edit->hasFocus()) {
            focusActivePanel();
        }
    });
}

void PanelSearchController::deactivateForNonMainView()
{
    m_mainViewActive = false;
    setVisible(false);
    updatePanelActiveProperties();
}

void PanelSearchController::refreshPanel(MainPanelId id)
{
    rebuildMatches(id, false, true);
}

void PanelSearchController::refreshActivePanel()
{
    refreshPanel(m_activePanel);
}

void PanelSearchController::setActivePanel(MainPanelId id, bool focus)
{
    if (targetForId(id) == nullptr) {
        return;
    }
    const bool changed = !m_hasActivePanel || m_activePanel != id;
    m_activePanel = id;
    m_hasActivePanel = true;
    if (changed) {
        if (MainPanelTarget *target = targetForId(id); target != nullptr && target->prepareForFocus) {
            target->prepareForFocus(focus);
        }
    }
    if (focus) {
        focusActivePanel();
    }
    updatePanelActiveProperties();
    updateSearchUi();
    if (changed) {
        emit activePanelChanged(id);
    }
}

void PanelSearchController::setActivePanelFromString(const QString &value)
{
    MainPanelId id = MainPanelId::Artists;
    if (mainPanelIdFromString(value, &id)) {
        setActivePanel(id, false);
    }
}

bool PanelSearchController::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_mainViewActive && watched != m_edit) {
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        return handleSearchKey(static_cast<QKeyEvent *>(event));
    }

    if (event->type() == QEvent::FocusIn) {
        if (QWidget *widget = eventWidgetFor(watched)) {
            if (MainPanelTarget *target = targetForWidget(widget)) {
                setActivePanel(target->id, false);
            }
        }
    }

    if (event->type() == QEvent::KeyPress) {
        if (QWidget *widget = eventWidgetFor(watched)) {
            if (MainPanelTarget *target = targetForWidget(widget)) {
                setActivePanel(target->id, false);
                return handlePanelKey(static_cast<QKeyEvent *>(event), target->id);
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

QWidget *PanelSearchController::eventWidgetFor(QObject *watched) const
{
    return qobject_cast<QWidget *>(watched);
}

void PanelSearchController::installTargetMappings(const MainPanelTarget &target)
{
    target.focusWidget->installEventFilter(this);
    m_widgetToPanel.insert(target.focusWidget, target.id);
    if (auto *view = qobject_cast<QAbstractItemView *>(target.focusWidget)) {
        view->viewport()->installEventFilter(this);
        m_widgetToPanel.insert(view->viewport(), target.id);
    }
}

void PanelSearchController::removeTargetMappings(const MainPanelTarget &target)
{
    if (target.focusWidget == nullptr) {
        return;
    }

    target.focusWidget->removeEventFilter(this);
    m_widgetToPanel.remove(target.focusWidget);
    if (auto *view = qobject_cast<QAbstractItemView *>(target.focusWidget)) {
        view->viewport()->removeEventFilter(this);
        m_widgetToPanel.remove(view->viewport());
    }
}

MainPanelTarget *PanelSearchController::targetForId(MainPanelId id)
{
    for (MainPanelTarget &target : m_targets) {
        if (target.id == id) {
            return &target;
        }
    }
    return nullptr;
}

const MainPanelTarget *PanelSearchController::targetForId(MainPanelId id) const
{
    for (const MainPanelTarget &target : m_targets) {
        if (target.id == id) {
            return &target;
        }
    }
    return nullptr;
}

MainPanelTarget *PanelSearchController::targetForWidget(QWidget *widget)
{
    if (widget == nullptr || !m_widgetToPanel.contains(widget)) {
        return nullptr;
    }
    return targetForId(m_widgetToPanel.value(widget));
}

void PanelSearchController::updatePanelActiveProperties()
{
    for (const MainPanelTarget &target : m_targets) {
        const bool active = m_mainViewActive && m_hasActivePanel && target.id == m_activePanel;
        if (auto *table = qobject_cast<NavigableTableView *>(target.focusWidget)) {
            table->setMainPanelActive(active);
        } else if (auto *view = qobject_cast<QAbstractItemView *>(target.focusWidget)) {
            target.focusWidget->setProperty("mainPanelActive", active);
            target.focusWidget->update();
            view->viewport()->setProperty("mainPanelActive", active);
            view->viewport()->update();
        } else {
            target.focusWidget->setProperty("mainPanelActive", active);
            target.focusWidget->update();
        }
    }
}

void PanelSearchController::focusActivePanel()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        target->focusWidget->setFocus();
    }
}

void PanelSearchController::openSearch()
{
    setVisible(true);
    updateSearchUi();
    const QSignalBlocker blocker(m_edit);
    m_edit->setText(m_state[m_activePanel].query);
    m_edit->setFocus();
    m_edit->selectAll();
}

void PanelSearchController::escapeSearch()
{
    PanelState &state = m_state[m_activePanel];
    if (!m_edit->text().isEmpty()) {
        m_edit->clear();
        return;
    }
    state.query.clear();
    state.matches.clear();
    state.currentMatch = -1;
    setVisible(false);
    focusActivePanel();
}

void PanelSearchController::confirmSearch()
{
    // ncmpcpp-style confirm: with matches, keep the query and current match so
    // M-n / M-p keep cycling after the bar closes; with no matches, drop the
    // query entirely so the panel closes free of any constraint.
    PanelState &state = m_state[m_activePanel];
    if (state.matches.isEmpty()) {
        state.query.clear();
        state.currentMatch = -1;
    }
    setVisible(false);
    focusActivePanel();
    updateSearchUi();
}

void PanelSearchController::setQueryForActivePanel(const QString &query)
{
    // Keep the raw text (don't trim) so spaces survive while typing — the parser
    // skips empty whitespace tokens, and trimming here would let updateSearchUi
    // resync the edit and swallow a trailing space on every keystroke.
    PanelState &state = m_state[m_activePanel];
    state.query = query;
    rebuildMatches(m_activePanel, true, false);
    updateSearchUi();
}

void PanelSearchController::rebuildMatches(MainPanelId id, bool jumpToFirst, bool showNoMatchNotice)
{
    MainPanelTarget *target = targetForId(id);
    if (target == nullptr) {
        return;
    }

    PanelState &state = m_state[id];
    state.matches.clear();
    state.currentMatch = -1;

    if (state.query.trimmed().isEmpty()) {
        updateSearchUi();
        return;
    }

    state.matches = Search::matchDocumentsInDisplayOrder(target->documents(), Search::SearchQuery::parse(state.query), m_fuzzyMode);
    if (!state.matches.isEmpty()) {
        int matchIndex = 0;
        const int current = target->currentRow();
        for (int i = 0; i < state.matches.size(); ++i) {
            if (state.matches.at(i).row >= current) {
                matchIndex = i;
                break;
            }
        }
        state.currentMatch = matchIndex;
        if (jumpToFirst) {
            target->setCurrentRow(state.matches.at(matchIndex).row);
        }
    } else if (showNoMatchNotice && id == m_activePanel) {
        emit statusMessage(QStringLiteral("No matches for '%1' in %2").arg(state.query, target->label), 3000);
    }
    updateSearchUi();
}

void PanelSearchController::updateSearchUi()
{
    const MainPanelTarget *target = targetForId(m_activePanel);
    const QString label = target != nullptr ? target->label : QStringLiteral("Panel");
    m_prompt->setText(QStringLiteral("Search %1:").arg(label));

    const PanelState state = m_state.value(m_activePanel);
    if (m_edit->text() != state.query && m_edit->hasFocus()) {
        const QSignalBlocker blocker(m_edit);
        m_edit->setText(state.query);
    }

    const QString mode = m_fuzzyMode ? QStringLiteral("fuzzy") : QStringLiteral("exact");
    const int rows = target != nullptr ? target->rowCount() : 0;
    if (rows == 0) {
        m_status->setText(QStringLiteral("No rows · %1").arg(mode));
    } else if (state.query.trimmed().isEmpty()) {
        m_status->setText(QStringLiteral("%1 rows · %2").arg(rows).arg(mode));
    } else if (state.matches.isEmpty()) {
        m_status->setText(QStringLiteral("No matches · %1").arg(mode));
    } else {
        m_status->setText(QStringLiteral("%1/%2 · %3")
                              .arg(state.currentMatch + 1)
                              .arg(state.matches.size())
                              .arg(mode));
    }
}

void PanelSearchController::moveCurrent(int delta)
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (target->rowCount() <= 0) {
            return;
        }
        const int row = target->currentRow() >= 0 ? target->currentRow() : 0;
        const int next = std::clamp(row + delta, 0, target->rowCount() - 1);
        if (target->setCurrentRowWithDirection) {
            target->setCurrentRowWithDirection(next, delta);
        } else {
            target->setCurrentRow(next);
        }
    }
}

void PanelSearchController::movePage(int direction)
{
    moveCurrent(direction * pageStepForActivePanel());
}

void PanelSearchController::focusRelative(int direction)
{
    const qsizetype index = m_focusOrder.indexOf(m_activePanel);
    if (index < 0) {
        return;
    }
    const qsizetype next = index + direction;
    if (next < 0 || next >= m_focusOrder.size()) {
        return;
    }
    setActivePanel(m_focusOrder.at(next), true);
}

void PanelSearchController::focusQueue()
{
    setActivePanel(MainPanelId::Queue, true);
}

void PanelSearchController::focusTracks()
{
    setActivePanel(MainPanelId::Tracks, true);
}

void PanelSearchController::activateCurrent()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (target->activateCurrent) {
            target->activateCurrent();
        }
    }
}

void PanelSearchController::playCurrentNow()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (target->playCurrentNow) {
            target->playCurrentNow();
        } else if (target->activateCurrent) {
            target->activateCurrent();
        }
    }
}

void PanelSearchController::addCurrentToQueue()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (!target->addCurrentToQueue) {
            return;
        }
        target->addCurrentToQueue();
        moveCurrent(+1);
    }
}

void PanelSearchController::addCurrentToPlaylist()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (target->addCurrentToPlaylist) {
            target->addCurrentToPlaylist();
        }
    }
}

void PanelSearchController::playNextCurrent()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (!target->playNextCurrent) {
            return;
        }
        target->playNextCurrent();
        moveCurrent(+1);
    }
}

void PanelSearchController::markCurrent()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (!target->markCurrent) {
            return;
        }
        target->markCurrent();
        moveCurrent(+1);
    }
}

void PanelSearchController::markAll()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (target->markAll) {
            target->markAll();
        }
    }
}

void PanelSearchController::unmarkCurrent()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (!target->unmarkCurrent) {
            return;
        }
        target->unmarkCurrent();
        moveCurrent(+1);
    }
}

void PanelSearchController::unmarkAll()
{
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        if (target->unmarkAll) {
            target->unmarkAll();
        }
    }
}

void PanelSearchController::cycleMatch(int direction)
{
    PanelState &state = m_state[m_activePanel];
    if (state.query.isEmpty()) {
        openSearch();
        return;
    }
    rebuildMatches(m_activePanel, false, true);
    if (state.matches.isEmpty()) {
        return;
    }
    if (state.currentMatch < 0) {
        state.currentMatch = 0;
    } else {
        const qsizetype next = (static_cast<qsizetype>(state.currentMatch) + direction + state.matches.size()) % state.matches.size();
        state.currentMatch = static_cast<int>(next);
    }
    if (MainPanelTarget *target = targetForId(m_activePanel)) {
        target->setCurrentRow(state.matches.at(state.currentMatch).row);
    }
    updateSearchUi();
}

bool PanelSearchController::handleAlbumGridKey(QKeyEvent *event, const QString &action)
{
    if (m_activePanel != MainPanelId::Albums) {
        return false;
    }
    MainPanelTarget *target = targetForId(MainPanelId::Albums);
    if (target == nullptr || !target->moveCurrentInGrid) {
        return false;
    }

    if (event->modifiers() == Qt::NoModifier) {
        if (event->key() == Qt::Key_N) {
            activateCurrent();
            focusTracks();
            return true;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            activateCurrent();
            return true;
        }
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            return true;
        }
    }

    if (action == QString::fromLatin1(MainPanelAction::PlayNow)) {
        if (event->modifiers() & Qt::AltModifier) {
            playCurrentNow();
        }
        return true;
    }
    if (action == QString::fromLatin1(MainPanelAction::MoveDown)) {
        target->moveCurrentInGrid(+1, 0);
        return true;
    }
    if (action == QString::fromLatin1(MainPanelAction::MoveUp)) {
        target->moveCurrentInGrid(-1, 0);
        return true;
    }
    return false;
}

bool PanelSearchController::handlePanelKey(QKeyEvent *event, MainPanelId panel)
{
    Q_UNUSED(panel)
    QString action = m_keyBindings.value(keySequenceForEvent(event));
    if (action.isEmpty() && event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_V) {
        action = QString::fromLatin1(MainPanelAction::PageDown);
    }
    if (action.isEmpty() && m_activePanel != MainPanelId::Albums) {
        return false;
    }

    if (handleAlbumGridKey(event, action)) {
        return true;
    }

    if (action.isEmpty()) {
        return false;
    }

    if (action == QString::fromLatin1(MainPanelAction::MoveDown)) moveCurrent(+1);
    else if (action == QString::fromLatin1(MainPanelAction::MoveUp)) moveCurrent(-1);
    else if (action == QString::fromLatin1(MainPanelAction::PageDown)) movePage(+1);
    else if (action == QString::fromLatin1(MainPanelAction::PageUp)) movePage(-1);
    else if (action == QString::fromLatin1(MainPanelAction::FocusPrevious)) {
        // h from Tracks returns to the album grid. A keyboard-made multi-album
        // selection is meant to survive going back and forth, so we keep it; every
        // other narrowing — a single album (n / click) or a mouse-made
        // multi-selection — counts as a finished interaction, so returning to the
        // grid restarts fresh and we clear it. Escape (below) always clears.
        const MainPanelId panelBeforeClear = m_activePanel;
        if (panelBeforeClear == MainPanelId::Tracks) {
            if (const MainPanelTarget *grid = targetForId(MainPanelId::Albums);
                grid != nullptr && grid->clearNarrowing
                && (!grid->narrowingPersistsOnReturn || !grid->narrowingPersistsOnReturn())) {
                grid->clearNarrowing();
                if (m_activePanel != panelBeforeClear) {
                    return true;
                }
            }
        }
        focusRelative(-1);
    }
    else if (action == QString::fromLatin1(MainPanelAction::FocusNext)) {
        focusRelative(+1);
    }
    else if (action == QString::fromLatin1(MainPanelAction::FocusQueue)) focusQueue();
    else if (action == QString::fromLatin1(MainPanelAction::FocusTracks)) focusTracks();
    else if (action == QString::fromLatin1(MainPanelAction::Activate)) activateCurrent();
    else if (action == QString::fromLatin1(MainPanelAction::PlayNow)) playCurrentNow();
    else if (action == QString::fromLatin1(MainPanelAction::AddToQueue)) addCurrentToQueue();
    else if (action == QString::fromLatin1(MainPanelAction::AddToPlaylist)) addCurrentToPlaylist();
    else if (action == QString::fromLatin1(MainPanelAction::PlayNext)) playNextCurrent();
    else if (action == QString::fromLatin1(MainPanelAction::Mark)) markCurrent();
    else if (action == QString::fromLatin1(MainPanelAction::MarkAll)) markAll();
    else if (action == QString::fromLatin1(MainPanelAction::Unmark)) unmarkCurrent();
    else if (action == QString::fromLatin1(MainPanelAction::UnmarkAll)) unmarkAll();
    else if (action == QString::fromLatin1(MainPanelAction::Search)) openSearch();
    else if (action == QString::fromLatin1(MainPanelAction::SearchNext)) cycleMatch(+1);
    else if (action == QString::fromLatin1(MainPanelAction::SearchPrevious)) cycleMatch(-1);
    else if (action == QString::fromLatin1(MainPanelAction::Escape)) {
        if (isVisible()) escapeSearch();
        else if (MainPanelTarget *target = targetForId(m_activePanel); target != nullptr && target->clearNarrowing) target->clearNarrowing();
        else return false;
    } else {
        return false;
    }
    return true;
}

bool PanelSearchController::handleSearchKey(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == 0) {
        confirmSearch();
        return true;
    }
    const QString action = m_keyBindings.value(keySequenceForEvent(event));
    if (action == QString::fromLatin1(MainPanelAction::Escape)) {
        escapeSearch();
        return true;
    }
    if (action == QString::fromLatin1(MainPanelAction::SearchNext)) {
        cycleMatch(+1);
        return true;
    }
    if (action == QString::fromLatin1(MainPanelAction::SearchPrevious)) {
        cycleMatch(-1);
        return true;
    }
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_F) {
        m_fuzzyMode = !m_fuzzyMode;
        rebuildMatches(m_activePanel, false, false);
        updateSearchUi();
        return true;
    }
    return false;
}

int PanelSearchController::pageStepForActivePanel() const
{
    const MainPanelTarget *target = targetForId(m_activePanel);
    if (target == nullptr || target->rowCount() <= 0) {
        return 1;
    }
    if (auto *view = qobject_cast<QAbstractItemView *>(target->focusWidget)) {
        const int current = std::max(0, target->currentRow());
        const int rowHeight = std::max(1, view->sizeHintForRow(current));
        if (rowHeight > 1) {
            return std::max(1, view->viewport()->height() / rowHeight);
        }
    }
    return std::max(1, target->rowCount() / 10);
}
