#include "ui/KeybindingsDialog.h"

#include "ui/FileExplorerKeybindings.h"
#include "ui/MainPanelKeybindings.h"
#include "ui/QueueKeybindings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

QString actionLabel(const QString &action)
{
    static const QMap<QString, QString> labels = {
        {QString::fromLatin1(MainPanelAction::MoveDown), QStringLiteral("Move down")},
        {QString::fromLatin1(MainPanelAction::MoveUp), QStringLiteral("Move up")},
        {QString::fromLatin1(MainPanelAction::PageDown), QStringLiteral("Page down")},
        {QString::fromLatin1(MainPanelAction::PageUp), QStringLiteral("Page up")},
        {QString::fromLatin1(MainPanelAction::FocusPrevious), QStringLiteral("Focus previous panel")},
        {QString::fromLatin1(MainPanelAction::FocusNext), QStringLiteral("Focus next panel")},
        {QString::fromLatin1(MainPanelAction::FocusQueue), QStringLiteral("Focus queue")},
        {QString::fromLatin1(MainPanelAction::FocusTracks), QStringLiteral("Focus tracks")},
        {QString::fromLatin1(MainPanelAction::Activate), QStringLiteral("Activate / narrow")},
        {QString::fromLatin1(MainPanelAction::PlayNow), QStringLiteral("Play now")},
        {QString::fromLatin1(MainPanelAction::AddToQueue), QStringLiteral("Add to queue")},
        {QString::fromLatin1(MainPanelAction::PlayNext), QStringLiteral("Play next")},
        {QString::fromLatin1(MainPanelAction::Mark), QStringLiteral("Mark item")},
        {QString::fromLatin1(MainPanelAction::MarkAll), QStringLiteral("Mark all")},
        {QString::fromLatin1(MainPanelAction::Unmark), QStringLiteral("Unmark item")},
        {QString::fromLatin1(MainPanelAction::UnmarkAll), QStringLiteral("Unmark all")},
        {QString::fromLatin1(MainPanelAction::Search), QStringLiteral("Panel search")},
        {QString::fromLatin1(MainPanelAction::SearchNext), QStringLiteral("Next match")},
        {QString::fromLatin1(MainPanelAction::SearchPrevious), QStringLiteral("Previous match")},
        {QString::fromLatin1(MainPanelAction::Escape), QStringLiteral("Escape / clear")},
        {QString::fromLatin1(ExplorerAction::NavigateUp), QStringLiteral("Navigate up")},
        {QString::fromLatin1(ExplorerAction::NavigateIn), QStringLiteral("Navigate in")},
        {QString::fromLatin1(ExplorerAction::ScrollToTop), QStringLiteral("Scroll to top")},
        {QString::fromLatin1(ExplorerAction::ScrollToBottom), QStringLiteral("Scroll to bottom")},
        {QString::fromLatin1(ExplorerAction::PlaySelected), QStringLiteral("Play selected")},
        {QString::fromLatin1(ExplorerAction::ImportDirectory), QStringLiteral("Import directory")},
        {QString::fromLatin1(ExplorerAction::FindFile), QStringLiteral("Find file")},
        {QString::fromLatin1(ExplorerAction::GoHome), QStringLiteral("Go home")},
        {QString::fromLatin1(ExplorerAction::GoToStart), QStringLiteral("Go to start")},
        {QString::fromLatin1(QueueAction::RemoveSelected), QStringLiteral("Remove selected")},
        {QString::fromLatin1(QueueAction::ClearQueue), QStringLiteral("Clear queue")},
        {QString::fromLatin1(QueueAction::ClearPlayNext), QStringLiteral("Clear play-next priority")},
        {QString::fromLatin1(QueueAction::FindLibrary), QStringLiteral("Find in library")},
        {QString::fromLatin1(QueueAction::JumpPlaying), QStringLiteral("Jump to currently playing")},
        {QString::fromLatin1(QueueAction::Search), QStringLiteral("Search queue")},
        {QString::fromLatin1(QueueAction::SearchNext), QStringLiteral("Next match")},
        {QString::fromLatin1(QueueAction::SearchPrevious), QStringLiteral("Previous match")},
    };
    return labels.value(action, action);
}

QString keyText(const QKeySequence &sequence)
{
    return sequence.toString(QKeySequence::NativeText);
}

void addBindingRow(QTableWidget *table, const QString &key, const QString &action)
{
    const int row = table->rowCount();
    table->insertRow(row);
    auto *keyItem = new QTableWidgetItem(key);
    auto *actionItem = new QTableWidgetItem(action);
    keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
    actionItem->setFlags(actionItem->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 0, keyItem);
    table->setItem(row, 1, actionItem);
}

void fillBindings(QTableWidget *table, const KeyBindingMap &bindings)
{
    table->setRowCount(0);
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        addBindingRow(table, keyText(it.key()), actionLabel(it.value()));
    }
    table->resizeRowsToContents();
}

KeyBindingMap bindingsForProfile(const QVector<KeyBindingProfile> &profiles, const QString &name)
{
    for (const KeyBindingProfile &profile : profiles) {
        if (profile.name == name) {
            return profile.bindings;
        }
    }
    return profiles.isEmpty() ? KeyBindingMap{} : profiles.first().bindings;
}

void addGlobalRows(QTableWidget *table, const QVector<std::pair<QString, QString>> &rows)
{
    if (table->rowCount() > 0) {
        table->insertRow(table->rowCount());
    }
    for (const auto &[key, action] : rows) {
        addBindingRow(table, key, action);
    }
}

} // namespace

KeybindingsDialog::KeybindingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Keybinds"));
    resize(640, 560);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);

    auto *mainPanelTab = new QWidget(tabs);
    auto *mainLayout = new QVBoxLayout(mainPanelTab);
    mainLayout->addWidget(new QLabel(QStringLiteral("Screen 1: Library panels"), mainPanelTab));
    m_mainPanelProfile = new QComboBox(mainPanelTab);
    for (const KeyBindingProfile &profile : defaultMainPanelKeyBindingProfiles()) {
        m_mainPanelProfile->addItem(profile.label, profile.name);
    }
    mainLayout->addWidget(m_mainPanelProfile);
    m_mainPanelBindings = makeBindingsTable(mainPanelTab);
    mainLayout->addWidget(m_mainPanelBindings, 1);
    tabs->addTab(mainPanelTab, QStringLiteral("1 Library"));
    connect(m_mainPanelProfile, &QComboBox::currentIndexChanged, this, &KeybindingsDialog::rebuildMainPanelBindings);

    auto *explorerTab = new QWidget(tabs);
    auto *explorerLayout = new QVBoxLayout(explorerTab);
    explorerLayout->addWidget(new QLabel(QStringLiteral("Screen 2: File explorer"), explorerTab));
    m_fileExplorerProfile = new QComboBox(explorerTab);
    for (const KeyBindingProfile &profile : defaultKeyBindingProfiles()) {
        m_fileExplorerProfile->addItem(profile.label, profile.name);
    }
    explorerLayout->addWidget(m_fileExplorerProfile);
    m_fileExplorerHints = new QCheckBox(QStringLiteral("Show key hints"), explorerTab);
    explorerLayout->addWidget(m_fileExplorerHints);
    m_fileExplorerBindings = makeBindingsTable(explorerTab);
    explorerLayout->addWidget(m_fileExplorerBindings, 1);
    tabs->addTab(explorerTab, QStringLiteral("2 Explorer"));
    connect(m_fileExplorerProfile, &QComboBox::currentIndexChanged, this, &KeybindingsDialog::rebuildFileExplorerBindings);

    auto *searchTab = new QWidget(tabs);
    auto *searchLayout = new QVBoxLayout(searchTab);
    searchLayout->addWidget(new QLabel(QStringLiteral("Screen 3: Search"), searchTab));
    auto *searchBindings = makeBindingsTable(searchTab);
    addGlobalRows(searchBindings, {
        {QStringLiteral("3"), QStringLiteral("Switch to Search / refresh Search")},
        {QStringLiteral("F5"), QStringLiteral("Refresh search index")},
        {QStringLiteral("Down / Ctrl+N"), QStringLiteral("Move down")},
        {QStringLiteral("Up / Ctrl+P"), QStringLiteral("Move up")},
        {QStringLiteral("PageDown / PageUp"), QStringLiteral("Page results")},
        {QStringLiteral("Return"), QStringLiteral("Add selected results to queue")},
        {QStringLiteral("Alt+Return"), QStringLiteral("Play selected results now")},
        {QStringLiteral("Tab / Ctrl+Space"), QStringLiteral("Toggle result mark")},
        {QStringLiteral("Ctrl+A"), QStringLiteral("Select all results")},
        {QStringLiteral("Ctrl+F"), QStringLiteral("Toggle fuzzy mode")},
        {QStringLiteral("Esc / Ctrl+G"), QStringLiteral("Clear search / leave input")},
    });
    searchLayout->addWidget(searchBindings, 1);
    tabs->addTab(searchTab, QStringLiteral("3 Search"));

    auto *queueTab = new QWidget(tabs);
    auto *queueLayout = new QVBoxLayout(queueTab);
    queueLayout->addWidget(new QLabel(QStringLiteral("Screen `: Queue"), queueTab));
    m_queueProfile = new QComboBox(queueTab);
    for (const KeyBindingProfile &profile : defaultQueueKeyBindingProfiles()) {
        m_queueProfile->addItem(profile.label, profile.name);
    }
    queueLayout->addWidget(m_queueProfile);
    m_queueBindings = makeBindingsTable(queueTab);
    queueLayout->addWidget(m_queueBindings, 1);
    tabs->addTab(queueTab, QStringLiteral("` Queue"));
    connect(m_queueProfile, &QComboBox::currentIndexChanged, this, &KeybindingsDialog::rebuildQueueBindings);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    rebuildMainPanelBindings();
    rebuildFileExplorerBindings();
    rebuildQueueBindings();
}

void KeybindingsDialog::setMainPanelProfileName(const QString &name)
{
    const int index = m_mainPanelProfile->findData(name);
    if (index >= 0) {
        m_mainPanelProfile->setCurrentIndex(index);
    }
}

QString KeybindingsDialog::mainPanelProfileName() const
{
    return m_mainPanelProfile->currentData().toString();
}

void KeybindingsDialog::setFileExplorerProfileName(const QString &name)
{
    const int index = m_fileExplorerProfile->findData(name);
    if (index >= 0) {
        m_fileExplorerProfile->setCurrentIndex(index);
    }
}

QString KeybindingsDialog::fileExplorerProfileName() const
{
    return m_fileExplorerProfile->currentData().toString();
}

void KeybindingsDialog::setQueueProfileName(const QString &name)
{
    const int index = m_queueProfile->findData(name);
    if (index >= 0) {
        m_queueProfile->setCurrentIndex(index);
    }
}

QString KeybindingsDialog::queueProfileName() const
{
    return m_queueProfile->currentData().toString();
}

void KeybindingsDialog::setFileExplorerKeyHintsVisible(bool visible)
{
    m_fileExplorerHints->setChecked(visible);
}

bool KeybindingsDialog::fileExplorerKeyHintsVisible() const
{
    return m_fileExplorerHints->isChecked();
}

QTableWidget *KeybindingsDialog::makeBindingsTable(QWidget *parent)
{
    auto *table = new QTableWidget(0, 2, parent);
    table->setHorizontalHeaderLabels({QStringLiteral("Key"), QStringLiteral("Action")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return table;
}

void KeybindingsDialog::rebuildMainPanelBindings()
{
    fillBindings(m_mainPanelBindings, bindingsForProfile(defaultMainPanelKeyBindingProfiles(), mainPanelProfileName()));
    addGlobalRows(m_mainPanelBindings, {
        {QStringLiteral("1"), QStringLiteral("Switch to Library panels")},
        {QStringLiteral("o"), QStringLiteral("Find current track in library")},
    });
}

void KeybindingsDialog::rebuildFileExplorerBindings()
{
    fillBindings(m_fileExplorerBindings, bindingsForProfile(defaultKeyBindingProfiles(), fileExplorerProfileName()));
    addGlobalRows(m_fileExplorerBindings, {
        {QStringLiteral("2"), QStringLiteral("Toggle Library/Free-roam explorer")},
    });
}

void KeybindingsDialog::rebuildQueueBindings()
{
    fillBindings(m_queueBindings, bindingsForProfile(defaultQueueKeyBindingProfiles(), queueProfileName()));
    addGlobalRows(m_queueBindings, {
        {QStringLiteral("`"), QStringLiteral("Switch to Queue / jump to currently playing")},
    });
}
