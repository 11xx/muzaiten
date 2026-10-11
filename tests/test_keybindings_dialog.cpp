#include "ui/FileExplorerKeybindings.h"
#include "ui/KeybindCatalog.h"
#include "ui/KeybindingsDialog.h"
#include "ui/MainPanelKeybindings.h"
#include "ui/QueueKeybindings.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

namespace {

const QString queue = QString::fromLatin1(KeyBindingScopeId::Queue);
const QString library = QString::fromLatin1(KeyBindingScopeId::Library);
const QString explorer = QString::fromLatin1(KeyBindingScopeId::Explorer);

QKeySequence key(const char *text)
{
    return QKeySequence(QString::fromLatin1(text));
}

QStringList visibleActions(const QTreeWidget *tree)
{
    QStringList actions;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *group = tree->topLevelItem(i);
        for (int j = 0; j < group->childCount(); ++j) {
            if (!group->isHidden() && !group->child(j)->isHidden()) {
                actions.push_back(group->child(j)->data(0, Qt::UserRole).toString());
            }
        }
    }
    return actions;
}

} // namespace

class KeybindingsDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void opensWithDefaultProfiles()
    {
        KeybindingsDialog dialog;
        dialog.setMainPanelProfileName(QStringLiteral("dired_hjkl"));
        dialog.setFileExplorerProfileName(QStringLiteral("dired_hjkl"));
        dialog.setFileExplorerKeyHintsVisible(true);

        QTimer::singleShot(0, &dialog, &QDialog::accept);
        QCOMPARE(dialog.exec(), int(QDialog::Accepted));
        QCOMPARE(dialog.mainPanelProfileName(), QStringLiteral("dired_hjkl"));
        QCOMPARE(dialog.fileExplorerProfileName(), QStringLiteral("dired_hjkl"));
        QVERIFY(dialog.fileExplorerKeyHintsVisible());
        QVERIFY(dialog.overrides(queue).isEmpty());
    }

    void everyPresetActionHasALabelOnItsScreen()
    {
        for (const KeyBindingScope &scope : editableKeyBindingScopes()) {
            for (const KeyBindingProfile &preset : scope.presets) {
                for (const QString &action : preset.bindings) {
                    QVERIFY2(keyActionLabel(scope, action) != action,
                             qPrintable(scope.id + QLatin1Char('/') + action));
                }
            }
        }
        // The same action id reads differently on each screen.
        QCOMPARE(keyActionLabel(*keyBindingScope(queue), QStringLiteral("search")), QStringLiteral("Search the queue"));
        QCOMPARE(keyActionLabel(*keyBindingScope(library), QStringLiteral("search")), QStringLiteral("Search this panel"));
    }

    void overridesReplaceAnActionsKeysAndTakeAssignedKeys()
    {
        const KeyBindingMap preset{{key("J"), QStringLiteral("move_down")},
                                   {key("N"), QStringLiteral("move_down")},
                                   {key("K"), QStringLiteral("move_up")},
                                   {key("C"), QStringLiteral("clear_queue")}};
        const KeyBindingOverrides overrides{{QStringLiteral("move_down"), {key("Ctrl+J")}},
                                            {QStringLiteral("clear_queue"), {}},
                                            {QStringLiteral("add_to_playlist"), {key("K")}}};
        const KeyBindingMap effective = applyKeyBindingOverrides(preset, overrides);
        QCOMPARE(keysForAction(effective, QStringLiteral("move_down")), QList<QKeySequence>{key("Ctrl+J")});
        QVERIFY(keysForAction(effective, QStringLiteral("clear_queue")).isEmpty());
        QCOMPARE(effective.value(key("K")), QStringLiteral("add_to_playlist"));
        QVERIFY(keysForAction(effective, QStringLiteral("move_up")).isEmpty());
        QVERIFY(!effective.contains(key("J")));
    }

    void overridesRoundTripThroughJsonAndDropBadEntries()
    {
        const KeyBindingScope &scope = *keyBindingScope(queue);
        const KeyBindingOverrides overrides{{QStringLiteral("move_item_down"), {key("Ctrl+J"), key("Shift+N")}},
                                            {QStringLiteral("clear_queue"), {}}};
        QCOMPARE(keyBindingOverridesFromJson(keyBindingOverridesToJson(overrides), scope), overrides);

        const QString hostile = QStringLiteral(
            R"({"move_item_down":["Ctrl+J","1","not a key","Ctrl+J"],"no_such_action":["X"],"clear_queue":"C"})");
        const KeyBindingOverrides parsed = keyBindingOverridesFromJson(hostile, scope);
        QCOMPARE(parsed.value(QStringLiteral("move_item_down")), QList<QKeySequence>{key("Ctrl+J")});
        QVERIFY(!parsed.contains(QStringLiteral("no_such_action")));
        QVERIFY(!parsed.contains(QStringLiteral("clear_queue")));
        QVERIFY(keyBindingOverridesFromJson(QStringLiteral("[1,2]"), scope).isEmpty());
    }

    void shiftedSymbolsMatchTheirBinding()
    {
        const KeyBindingMap bindings = applyKeyBindingOverrides(bindingMapForProfile(QStringLiteral("dired")), {});
        QKeyEvent exclam(QEvent::KeyPress, Qt::Key_Exclam, Qt::ShiftModifier, QStringLiteral("!"));
        QCOMPARE(keyBindingAction(bindings, &exclam), QString::fromLatin1(ExplorerAction::PlayNext));
        QKeyEvent tilde(QEvent::KeyPress, Qt::Key_AsciiTilde, Qt::ShiftModifier, QStringLiteral("~"));
        QCOMPARE(keyBindingAction(bindings, &tilde), QString::fromLatin1(ExplorerAction::GoHome));
        QKeyEvent shiftN(QEvent::KeyPress, Qt::Key_N, Qt::ShiftModifier, QStringLiteral("N"));
        const KeyBindingMap queueBindings = queueBindingMapForProfile(QStringLiteral("dired_hjkl"));
        QCOMPARE(keyBindingAction(queueBindings, &shiftN), QString::fromLatin1(QueueAction::MoveItemDown));
    }

    void assigningAFreeKeyAddsItToTheAction()
    {
        KeybindingsDialog dialog;
        const QString action = QString::fromLatin1(QueueAction::AddToPlaylist);
        QCOMPARE(dialog.requestKey(queue, action, key("Ctrl+L")), KeybindingsDialog::KeyRequest::Assigned);
        QCOMPARE(keysForAction(dialog.effectiveBindings(queue), action), (QList<QKeySequence>{key("A"), key("Ctrl+L")}));
        QVERIFY(dialog.overrides(queue).contains(action));

        // Removing the added key brings the action back to its preset, so no
        // change is stored.
        dialog.removeKey(queue, action, key("Ctrl+L"));
        QVERIFY(dialog.overrides(queue).isEmpty());

        // An action can be left with no key at all.
        dialog.removeKey(queue, action, key("A"));
        QVERIFY(keysForAction(dialog.effectiveBindings(queue), action).isEmpty());
        QCOMPARE(dialog.overrides(queue).value(action), QList<QKeySequence>{});
        dialog.resetAction(queue, action);
        QVERIFY(dialog.overrides(queue).isEmpty());
    }

    void aKeyInUseMovesOnlyWhenConfirmed()
    {
        KeybindingsDialog dialog;
        const QString moveItem = QString::fromLatin1(QueueAction::MoveItemDown);
        const QString nextMatch = QString::fromLatin1(QueueAction::SearchNext);
        QCOMPARE(dialog.requestKey(queue, moveItem, key("Alt+N")), KeybindingsDialog::KeyRequest::Conflict);
        QCOMPARE(dialog.effectiveBindings(queue).value(key("Alt+N")), nextMatch);

        dialog.keepElsewhere(queue);
        QCOMPARE(dialog.effectiveBindings(queue).value(key("Alt+N")), nextMatch);
        QVERIFY(dialog.overrides(queue).isEmpty());

        QCOMPARE(dialog.requestKey(queue, moveItem, key("Alt+N")), KeybindingsDialog::KeyRequest::Conflict);
        dialog.confirmUseHere(queue);
        const KeyBindingMap bindings = dialog.effectiveBindings(queue);
        QCOMPARE(bindings.value(key("Alt+N")), moveItem);
        QVERIFY(keysForAction(bindings, nextMatch).isEmpty());
        QVERIFY(keysForAction(bindings, moveItem).contains(key("Shift+N")));
    }

    void windowKeysAreRefused()
    {
        KeybindingsDialog dialog;
        const QString action = QString::fromLatin1(MainPanelAction::FocusTracks);
        QCOMPARE(dialog.requestKey(library, action, key("2")), KeybindingsDialog::KeyRequest::Reserved);
        QCOMPARE(dialog.requestKey(library, action, key("S")), KeybindingsDialog::KeyRequest::Reserved);
        QVERIFY(dialog.overrides(library).isEmpty());
        QCOMPARE(dialog.requestKey(library, action, key("Shift+S")), KeybindingsDialog::KeyRequest::Assigned);
        QCOMPARE(dialog.requestKey(library, action, key("Shift+S")), KeybindingsDialog::KeyRequest::AlreadyAssigned);
    }

    void restoringDefaultsClearsChangesAndThePreset()
    {
        KeybindingsDialog dialog;
        dialog.setFileExplorerProfileName(QStringLiteral("dired"));
        dialog.requestKey(explorer, QString::fromLatin1(ExplorerAction::GoHome), key("Ctrl+H"));
        QVERIFY(!dialog.overrides(explorer).isEmpty());
        dialog.restoreDefaults(explorer);
        QVERIFY(dialog.overrides(explorer).isEmpty());
        QCOMPARE(dialog.fileExplorerProfileName(), QStringLiteral("vim"));
    }

    void lookingUpAKeyShowsWhatItRuns()
    {
        KeybindingsDialog dialog;
        dialog.selectScreen(queue);
        auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("bindings-queue"));
        QVERIFY(tree != nullptr);
        dialog.lookUpKey(key("Shift+P"));
        QCOMPARE(visibleActions(tree), QStringList{QString::fromLatin1(QueueAction::MoveItemUp)});
        dialog.lookUpKey(QKeySequence());
        QVERIFY(visibleActions(tree).size() > 10);
    }
};

QTEST_MAIN(KeybindingsDialogTest)

#include "test_keybindings_dialog.moc"
