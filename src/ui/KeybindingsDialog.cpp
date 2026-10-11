#include "ui/KeybindingsDialog.h"

#include "ui/PlaylistView.h"
#include "ui/SearchView.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int ActionRole = Qt::UserRole;

QLabel *pageHeading(const QString &text, QWidget *parent)
{
    auto *heading = new QLabel(text, parent);
    QFont font = heading->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.25);
    heading->setFont(font);
    return heading;
}

QTreeWidget *bindingTree(QWidget *parent)
{
    auto *tree = new QTreeWidget(parent);
    tree->setColumnCount(2);
    tree->setHeaderLabels({QStringLiteral("Action"), QStringLiteral("Keys")});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setItemsExpandable(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree->header()->setSectionsMovable(false);
    return tree;
}

QTreeWidgetItem *headingItem(QTreeWidget *tree, const QString &text)
{
    auto *item = new QTreeWidgetItem(tree, {text});
    QFont font = item->font(0);
    font.setBold(true);
    item->setFont(0, font);
    item->setFirstColumnSpanned(true);
    item->setFlags(Qt::ItemIsEnabled);
    return item;
}

QString joinedKeys(const QList<QKeySequence> &keys)
{
    QStringList texts;
    for (const QKeySequence &key : keys) {
        texts.push_back(keyText(key));
    }
    return texts.join(QStringLiteral(", "));
}

QSet<QKeySequence> keySet(const QList<QKeySequence> &keys)
{
    return QSet<QKeySequence>(keys.cbegin(), keys.cend());
}

bool isModifierKey(int key)
{
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_AltGr || key == Qt::Key_Super_L || key == Qt::Key_Super_R;
}

} // namespace

struct KeybindingsDialog::Page {
    const KeyBindingScope *scope = nullptr;
    QComboBox *preset = nullptr;
    QCheckBox *hints = nullptr;
    QLineEdit *filter = nullptr;
    QPushButton *find = nullptr;
    QTreeWidget *tree = nullptr;
    QLabel *actionName = nullptr;
    QHBoxLayout *chips = nullptr;
    QToolButton *addKey = nullptr;
    QKeySequenceEdit *capture = nullptr;
    QPushButton *resetAction = nullptr;
    QWidget *notice = nullptr;
    QLabel *noticeText = nullptr;
    QPushButton *useHere = nullptr;
    QPushButton *keepThere = nullptr;
    KeyBindingOverrides overrides;
    QString selectedAction;
    QString pendingAction;
    QKeySequence pendingKey;
    QKeySequence lookupKey;
    // A refused key's explanation, shown while its action stays selected.
    QString refusal;
    QString refusalAction;

    KeyBindingMap presetOnly() const
    {
        return applyKeyBindingOverrides(presetBindings(*scope, preset->currentData().toString()), {});
    }
    KeyBindingMap effective() const
    {
        return applyKeyBindingOverrides(presetBindings(*scope, preset->currentData().toString()), overrides);
    }
};

KeybindingsDialog::KeybindingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Keybinds"));
    resize(820, 600);

    auto *root = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    m_screens = new QListWidget(this);
    m_screens->setFixedWidth(150);
    m_pages = new QStackedWidget(this);
    body->addWidget(m_screens);
    body->addWidget(m_pages, 1);
    root->addLayout(body, 1);

    for (const KeyBindingScope &scope : editableKeyBindingScopes()) {
        auto *page = new Page;
        page->scope = &scope;
        m_editablePages.push_back(page);
        m_screens->addItem(scope.label);
        m_screenIds.push_back(scope.id);
        m_pages->addWidget(buildEditablePage(*page));
    }
    const std::pair<QString, KeyBindingReferenceList> fixedScreens[] = {
        {QStringLiteral("Search"), SearchView::keyBindingReference()},
        {QStringLiteral("Playlists"), PlaylistView::keyBindingReference()},
        {QStringLiteral("Global"), windowKeyBindingReference()},
    };
    for (const auto &[title, rows] : fixedScreens) {
        m_screens->addItem(title);
        m_screenIds.push_back(title.toLower());
        m_pages->addWidget(buildFixedPage(title, rows));
    }
    connect(m_screens, &QListWidget::currentRowChanged, this, [this](int row) {
        setFinding(false);
        m_pages->setCurrentIndex(row);
        updateFooter();
    });

    auto *footer = new QHBoxLayout;
    m_restoreDefaults = new QPushButton(this);
    connect(m_restoreDefaults, &QPushButton::clicked, this, [this]() {
        if (Page *page = currentPage()) {
            restoreDefaults(page->scope->id);
        }
    });
    footer->addWidget(m_restoreDefaults);
    footer->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    footer->addWidget(buttons);
    root->addLayout(footer);

    m_screens->setCurrentRow(0);
    updateFooter();
}

KeybindingsDialog::~KeybindingsDialog()
{
    setFinding(false);
    qDeleteAll(m_editablePages);
}

QWidget *KeybindingsDialog::buildEditablePage(Page &page)
{
    auto *widget = new QWidget(m_pages);
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(pageHeading(page.scope->label, widget));
    titleRow->addStretch(1);
    auto *presetLabel = new QLabel(QStringLiteral("Preset"), widget);
    page.preset = new QComboBox(widget);
    for (const KeyBindingProfile &preset : page.scope->presets) {
        page.preset->addItem(preset.label, preset.name);
    }
    page.preset->setCurrentIndex(std::max(0, page.preset->findData(page.scope->defaultPreset)));
    presetLabel->setBuddy(page.preset);
    titleRow->addWidget(presetLabel);
    titleRow->addWidget(page.preset);
    layout->addLayout(titleRow);
    connect(page.preset, &QComboBox::currentIndexChanged, this, [this, &page]() {
        page.pendingAction.clear();
        refreshTree(page);
    });

    if (page.scope->id == QString::fromLatin1(KeyBindingScopeId::Explorer)) {
        page.hints = new QCheckBox(QStringLiteral("Show key hints below the file explorer"), widget);
        layout->addWidget(page.hints);
    }

    auto *searchRow = new QHBoxLayout;
    page.filter = new QLineEdit(widget);
    page.filter->setPlaceholderText(QStringLiteral("Filter actions"));
    page.filter->setClearButtonEnabled(true);
    searchRow->addWidget(page.filter, 1);
    page.find = new QPushButton(QStringLiteral("Find a key…"), widget);
    page.find->setCheckable(true);
    page.find->setToolTip(QStringLiteral("Press a key to see what it does on this screen"));
    searchRow->addWidget(page.find);
    layout->addLayout(searchRow);
    connect(page.filter, &QLineEdit::textEdited, this, [this, &page]() {
        page.lookupKey = QKeySequence();
        applyFilter(page);
    });
    connect(page.find, &QPushButton::toggled, this, [this](bool checked) { setFinding(checked); });

    page.tree = bindingTree(widget);
    page.tree->setObjectName(QStringLiteral("bindings-%1").arg(page.scope->id));
    layout->addWidget(page.tree, 1);
    connect(page.tree, &QTreeWidget::currentItemChanged, this, [this, &page](QTreeWidgetItem *item) {
        const QString action = item != nullptr ? item->data(0, ActionRole).toString() : QString();
        if (!action.isEmpty() && action != page.selectedAction) {
            page.selectedAction = action;
            page.pendingAction.clear();
            refreshEditor(page);
        }
    });
    connect(page.tree, &QTreeWidget::itemActivated, this, [this, &page](QTreeWidgetItem *item) {
        if (item != nullptr && !item->data(0, ActionRole).toString().isEmpty()) {
            startCapture(page);
        }
    });

    page.actionName = new QLabel(widget);
    QFont actionFont = page.actionName->font();
    actionFont.setBold(true);
    page.actionName->setFont(actionFont);
    layout->addWidget(page.actionName);

    auto *keysRow = new QHBoxLayout;
    page.chips = new QHBoxLayout;
    page.chips->setSpacing(6);
    keysRow->addLayout(page.chips);
    page.addKey = new QToolButton(widget);
    page.addKey->setText(QStringLiteral("Add key"));
    page.addKey->setToolTip(QStringLiteral("Press a key to add it to this action"));
    keysRow->addWidget(page.addKey);
    page.capture = new QKeySequenceEdit(widget);
    page.capture->setMaximumSequenceLength(1);
    page.capture->setFixedWidth(140);
    page.capture->setToolTip(QStringLiteral("Press the new key"));
    page.capture->hide();
    page.capture->installEventFilter(this);
    keysRow->addWidget(page.capture);
    keysRow->addStretch(1);
    page.resetAction = new QPushButton(QStringLiteral("Reset to preset"), widget);
    keysRow->addWidget(page.resetAction);
    layout->addLayout(keysRow);
    connect(page.addKey, &QToolButton::clicked, this, [this, &page]() { startCapture(page); });
    connect(page.capture, &QKeySequenceEdit::editingFinished, this, [this, &page]() { finishCapture(page); });
    connect(page.resetAction, &QPushButton::clicked, this, [this, &page]() {
        resetAction(page.scope->id, page.selectedAction);
    });

    page.notice = new QWidget(widget);
    auto *noticeRow = new QHBoxLayout(page.notice);
    noticeRow->setContentsMargins(0, 0, 0, 0);
    auto *icon = new QLabel(page.notice);
    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(iconSize, iconSize));
    noticeRow->addWidget(icon, 0, Qt::AlignTop);
    page.noticeText = new QLabel(page.notice);
    page.noticeText->setWordWrap(true);
    noticeRow->addWidget(page.noticeText, 1);
    page.useHere = new QPushButton(QStringLiteral("Use it here"), page.notice);
    page.keepThere = new QPushButton(QStringLiteral("Keep it there"), page.notice);
    noticeRow->addWidget(page.useHere);
    noticeRow->addWidget(page.keepThere);
    page.notice->hide();
    layout->addWidget(page.notice);
    connect(page.useHere, &QPushButton::clicked, this, [this, &page]() { confirmUseHere(page.scope->id); });
    connect(page.keepThere, &QPushButton::clicked, this, [this, &page]() { keepElsewhere(page.scope->id); });

    refreshTree(page);
    return widget;
}

QWidget *KeybindingsDialog::buildFixedPage(const QString &title, const KeyBindingReferenceList &rows)
{
    auto *widget = new QWidget(m_pages);
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(pageHeading(title, widget));
    auto *note = new QLabel(QStringLiteral("These keys are fixed."), widget);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);

    QTreeWidget *tree = bindingTree(widget);
    QTreeWidgetItem *group = nullptr;
    for (const auto &[key, action] : rows) {
        if (key.isEmpty()) {
            group = headingItem(tree, action);
            continue;
        }
        const QStringList columns{action, key};
        if (group != nullptr) {
            new QTreeWidgetItem(group, columns);
        } else {
            new QTreeWidgetItem(tree, columns);
        }
    }
    tree->expandAll();
    layout->addWidget(tree, 1);
    return widget;
}

KeybindingsDialog::Page *KeybindingsDialog::page(const QString &scopeId) const
{
    for (Page *page : m_editablePages) {
        if (page->scope->id == scopeId) {
            return page;
        }
    }
    return nullptr;
}

KeybindingsDialog::Page *KeybindingsDialog::currentPage() const
{
    const int row = m_screens->currentRow();
    return row >= 0 && row < m_editablePages.size() ? m_editablePages.at(row) : nullptr;
}

void KeybindingsDialog::refreshTree(Page &page)
{
    const KeyBindingMap bindings = page.effective();
    const QColor muted = page.tree->palette().color(QPalette::PlaceholderText);
    const QSignalBlocker blocker(page.tree);
    page.tree->clear();

    QHash<QString, QTreeWidgetItem *> groups;
    QTreeWidgetItem *selected = nullptr;
    QTreeWidgetItem *first = nullptr;
    for (const KeyActionInfo &action : page.scope->actions) {
        QTreeWidgetItem *&group = groups[action.group];
        if (group == nullptr) {
            group = headingItem(page.tree, action.group);
        }
        const QList<QKeySequence> keys = keysForAction(bindings, action.id);
        auto *item = new QTreeWidgetItem(group, {action.label, keys.isEmpty() ? QStringLiteral("Not assigned") : joinedKeys(keys)});
        item->setData(0, ActionRole, action.id);
        if (keys.isEmpty()) {
            item->setForeground(1, muted);
        }
        if (page.overrides.contains(action.id)) {
            QFont font = item->font(1);
            font.setBold(true);
            item->setFont(1, font);
            item->setToolTip(1, QStringLiteral("Changed from the preset"));
        }
        if (first == nullptr) {
            first = item;
        }
        if (action.id == page.selectedAction) {
            selected = item;
        }
    }
    page.tree->expandAll();
    if (selected == nullptr) {
        selected = first;
    }
    if (selected != nullptr) {
        page.tree->setCurrentItem(selected);
        page.selectedAction = selected->data(0, ActionRole).toString();
    }
    applyFilter(page);
    refreshEditor(page);
}

void KeybindingsDialog::refreshEditor(Page &page)
{
    const KeyBindingMap bindings = page.effective();
    const QString action = page.selectedAction;
    page.actionName->setText(action.isEmpty() ? QStringLiteral("Choose an action") : keyActionLabel(*page.scope, action));

    while (QLayoutItem *item = page.chips->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const QKeySequence &key : keysForAction(bindings, action)) {
        auto *chip = new QToolButton(page.addKey->parentWidget());
        chip->setText(keyText(key));
        chip->setIcon(style()->standardIcon(QStyle::SP_LineEditClearButton));
        chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        chip->setLayoutDirection(Qt::RightToLeft);
        chip->setToolTip(QStringLiteral("Remove %1").arg(keyText(key)));
        chip->setAccessibleName(QStringLiteral("Remove %1").arg(keyText(key)));
        connect(chip, &QToolButton::clicked, this, [this, &page, action, key]() {
            removeKey(page.scope->id, action, key);
        });
        page.chips->addWidget(chip);
    }
    page.addKey->setEnabled(!action.isEmpty());
    page.resetAction->setEnabled(page.overrides.contains(action));

    const bool conflict = !page.pendingAction.isEmpty() && page.pendingAction == action;
    if (conflict) {
        const QString owner = bindings.value(page.pendingKey);
        page.noticeText->setText(QStringLiteral("%1 already does %2. Use it here instead?")
                                     .arg(keyText(page.pendingKey), keyActionLabel(*page.scope, owner)));
    }
    const bool refused = !conflict && !page.refusal.isEmpty() && page.refusalAction == action;
    if (refused) {
        page.noticeText->setText(page.refusal);
    }
    page.useHere->setVisible(conflict);
    page.keepThere->setVisible(conflict);
    page.notice->setVisible(conflict || refused);
}

void KeybindingsDialog::applyFilter(Page &page)
{
    const QString text = page.filter->text().trimmed();
    const KeyBindingMap bindings = page.effective();
    const QString lookupAction = page.lookupKey.isEmpty() ? QString() : bindings.value(page.lookupKey);
    QTreeWidgetItem *firstVisible = nullptr;
    for (int i = 0; i < page.tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *group = page.tree->topLevelItem(i);
        bool anyVisible = false;
        for (int j = 0; j < group->childCount(); ++j) {
            QTreeWidgetItem *item = group->child(j);
            const QString action = item->data(0, ActionRole).toString();
            bool visible = true;
            if (!page.lookupKey.isEmpty()) {
                visible = action == lookupAction;
            } else if (!text.isEmpty()) {
                visible = item->text(0).contains(text, Qt::CaseInsensitive)
                    || (!keysForAction(bindings, action).isEmpty() && item->text(1).contains(text, Qt::CaseInsensitive));
            }
            item->setHidden(!visible);
            anyVisible = anyVisible || visible;
            if (visible && firstVisible == nullptr) {
                firstVisible = item;
            }
        }
        group->setHidden(!anyVisible);
    }
    if (!page.lookupKey.isEmpty() && firstVisible != nullptr) {
        page.tree->setCurrentItem(firstVisible);
    }
}

void KeybindingsDialog::setOverride(Page &page, const QString &action, const QList<QKeySequence> &keys)
{
    if (keySet(keys) == keySet(keysForAction(page.presetOnly(), action))) {
        page.overrides.remove(action);
    } else {
        page.overrides.insert(action, keys);
    }
}

KeybindingsDialog::KeyRequest KeybindingsDialog::requestKey(const QString &scopeId, const QString &action,
                                                            const QKeySequence &key)
{
    Page *page = this->page(scopeId);
    const QKeySequence normalized = normalizedKey(key.count() > 0 ? QKeySequence(key[0]) : QKeySequence());
    if (page == nullptr || action.isEmpty() || normalized.isEmpty()) {
        return KeyRequest::AlreadyAssigned;
    }
    page->pendingAction.clear();
    page->pendingKey = QKeySequence();
    page->refusal.clear();
    page->selectedAction = action;

    if (const QString use = reservedWindowKeyUse(normalized); !use.isEmpty()) {
        page->refusal = QStringLiteral("%1 is a window key (%2), so this screen never receives it. "
                                       "Choose another key.")
                            .arg(keyText(normalized), use);
        page->refusalAction = action;
        refreshTree(*page);
        return KeyRequest::Reserved;
    }

    const KeyBindingMap bindings = page->effective();
    const QString owner = bindings.value(normalized);
    if (owner == action) {
        refreshTree(*page);
        return KeyRequest::AlreadyAssigned;
    }
    if (!owner.isEmpty()) {
        page->pendingAction = action;
        page->pendingKey = normalized;
        refreshTree(*page);
        return KeyRequest::Conflict;
    }
    setOverride(*page, action, keysForAction(bindings, action) << normalized);
    refreshTree(*page);
    return KeyRequest::Assigned;
}

void KeybindingsDialog::confirmUseHere(const QString &scopeId)
{
    Page *page = this->page(scopeId);
    if (page == nullptr || page->pendingAction.isEmpty()) {
        return;
    }
    const KeyBindingMap bindings = page->effective();
    const QString owner = bindings.value(page->pendingKey);
    QList<QKeySequence> ownerKeys = keysForAction(bindings, owner);
    ownerKeys.removeAll(page->pendingKey);
    const QList<QKeySequence> actionKeys = keysForAction(bindings, page->pendingAction) << page->pendingKey;
    if (!owner.isEmpty()) {
        setOverride(*page, owner, ownerKeys);
    }
    setOverride(*page, page->pendingAction, actionKeys);
    page->pendingAction.clear();
    page->pendingKey = QKeySequence();
    refreshTree(*page);
}

void KeybindingsDialog::keepElsewhere(const QString &scopeId)
{
    if (Page *page = this->page(scopeId)) {
        page->pendingAction.clear();
        page->pendingKey = QKeySequence();
        refreshEditor(*page);
    }
}

void KeybindingsDialog::removeKey(const QString &scopeId, const QString &action, const QKeySequence &key)
{
    if (Page *page = this->page(scopeId)) {
        QList<QKeySequence> keys = keysForAction(page->effective(), action);
        keys.removeAll(normalizedKey(key));
        setOverride(*page, action, keys);
        page->pendingAction.clear();
        refreshTree(*page);
    }
}

void KeybindingsDialog::resetAction(const QString &scopeId, const QString &action)
{
    if (Page *page = this->page(scopeId)) {
        page->overrides.remove(action);
        page->pendingAction.clear();
        refreshTree(*page);
    }
}

void KeybindingsDialog::restoreDefaults(const QString &scopeId)
{
    if (Page *page = this->page(scopeId)) {
        page->overrides.clear();
        page->pendingAction.clear();
        const QSignalBlocker blocker(page->preset);
        page->preset->setCurrentIndex(std::max(0, page->preset->findData(page->scope->defaultPreset)));
        refreshTree(*page);
    }
}

void KeybindingsDialog::lookUpKey(const QKeySequence &key)
{
    Page *page = currentPage();
    if (page == nullptr) {
        return;
    }
    page->lookupKey = key.isEmpty() ? QKeySequence() : normalizedKey(key);
    page->filter->setText(page->lookupKey.isEmpty() ? QString() : keyText(page->lookupKey));
    applyFilter(*page);
}

void KeybindingsDialog::selectScreen(const QString &id)
{
    if (const qsizetype row = m_screenIds.indexOf(id); row >= 0) {
        m_screens->setCurrentRow(static_cast<int>(row));
    }
}

void KeybindingsDialog::selectAction(const QString &scopeId, const QString &action)
{
    if (Page *page = this->page(scopeId)) {
        page->selectedAction = action;
        refreshTree(*page);
    }
}

void KeybindingsDialog::startCapture(Page &page)
{
    if (page.selectedAction.isEmpty()) {
        return;
    }
    page.addKey->hide();
    page.capture->clear();
    page.capture->show();
    page.capture->setFocus(Qt::OtherFocusReason);
}

void KeybindingsDialog::finishCapture(Page &page)
{
    if (page.capture->isHidden()) {
        return;
    }
    const QKeySequence key = page.capture->keySequence();
    page.capture->hide();
    page.addKey->show();
    if (!key.isEmpty()) {
        requestKey(page.scope->id, page.selectedAction, key);
    }
    page.tree->setFocus(Qt::OtherFocusReason);
}

void KeybindingsDialog::setFinding(bool finding)
{
    if (m_finding == finding) {
        return;
    }
    m_finding = finding;
    for (Page *page : m_editablePages) {
        const QSignalBlocker blocker(page->find);
        page->find->setChecked(finding && page == currentPage());
        page->filter->setReadOnly(finding);
        page->filter->setPlaceholderText(finding ? QStringLiteral("Press a key to look it up. Esc stops.")
                                                 : QStringLiteral("Filter actions"));
    }
    if (finding) {
        if (Page *page = currentPage()) {
            page->filter->clear();
            page->lookupKey = QKeySequence();
            applyFilter(*page);
        }
        qApp->installEventFilter(this);
    } else {
        qApp->removeEventFilter(this);
    }
}

bool KeybindingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (m_finding && event->type() == QEvent::KeyPress) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (isModifierKey(keyEvent->key())) {
            return true;
        }
        const bool escape = keyEvent->key() == Qt::Key_Escape && keyEvent->modifiers() == Qt::NoModifier;
        setFinding(false);
        lookUpKey(escape ? QKeySequence() : keySequenceForEvent(keyEvent));
        return true;
    }
    if (event->type() == QEvent::FocusOut) {
        for (Page *page : m_editablePages) {
            if (watched == page->capture && page->capture->keySequence().isEmpty()) {
                page->capture->hide();
                page->addKey->show();
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void KeybindingsDialog::updateFooter()
{
    const Page *page = currentPage();
    m_restoreDefaults->setVisible(page != nullptr);
    if (page != nullptr) {
        m_restoreDefaults->setText(QStringLiteral("Restore the %1 defaults").arg(page->scope->label));
    }
}

void KeybindingsDialog::setMainPanelProfileName(const QString &name)
{
    if (Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Library))) {
        if (const int index = page->preset->findData(name); index >= 0) {
            page->preset->setCurrentIndex(index);
        }
    }
}

QString KeybindingsDialog::mainPanelProfileName() const
{
    const Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Library));
    return page != nullptr ? page->preset->currentData().toString() : QString();
}

void KeybindingsDialog::setFileExplorerProfileName(const QString &name)
{
    if (Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Explorer))) {
        if (const int index = page->preset->findData(name); index >= 0) {
            page->preset->setCurrentIndex(index);
        }
    }
}

QString KeybindingsDialog::fileExplorerProfileName() const
{
    const Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Explorer));
    return page != nullptr ? page->preset->currentData().toString() : QString();
}

void KeybindingsDialog::setQueueProfileName(const QString &name)
{
    if (Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Queue))) {
        if (const int index = page->preset->findData(name); index >= 0) {
            page->preset->setCurrentIndex(index);
        }
    }
}

QString KeybindingsDialog::queueProfileName() const
{
    const Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Queue));
    return page != nullptr ? page->preset->currentData().toString() : QString();
}

void KeybindingsDialog::setFileExplorerKeyHintsVisible(bool visible)
{
    if (Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Explorer)); page != nullptr && page->hints != nullptr) {
        page->hints->setChecked(visible);
    }
}

bool KeybindingsDialog::fileExplorerKeyHintsVisible() const
{
    const Page *page = this->page(QString::fromLatin1(KeyBindingScopeId::Explorer));
    return page != nullptr && page->hints != nullptr && page->hints->isChecked();
}

void KeybindingsDialog::setOverrides(const QString &scopeId, const KeyBindingOverrides &overrides)
{
    if (Page *page = this->page(scopeId)) {
        page->overrides = overrides;
        refreshTree(*page);
    }
}

KeyBindingOverrides KeybindingsDialog::overrides(const QString &scopeId) const
{
    const Page *page = this->page(scopeId);
    return page != nullptr ? page->overrides : KeyBindingOverrides{};
}

KeyBindingMap KeybindingsDialog::effectiveBindings(const QString &scopeId) const
{
    const Page *page = this->page(scopeId);
    return page != nullptr ? page->effective() : KeyBindingMap{};
}
