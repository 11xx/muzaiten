#include "ui/KeybindingsDialog.h"

#include <QTest>
#include <QTimer>

class KeybindingsDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void opensWithDefaultProfiles();
};

void KeybindingsDialogTest::opensWithDefaultProfiles()
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
}

QTEST_MAIN(KeybindingsDialogTest)

#include "test_keybindings_dialog.moc"
