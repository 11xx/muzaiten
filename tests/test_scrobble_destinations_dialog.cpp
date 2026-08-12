#include "scrobble/ListenBrainzUrl.h"
#include "scrobble/ScrobbleDestination.h"
#include "ui/ScrobbleDestinationsDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QTest>

class ScrobbleDestinationsDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void officialListenBrainzTokenCanBeReplaced();
};

void ScrobbleDestinationsDialogTest::officialListenBrainzTokenCanBeReplaced()
{
    QString writtenId;
    QString writtenToken;
    ScrobbleDestinationsDialog::Callbacks callbacks;
    callbacks.readToken = [](const QString &) { return QStringLiteral("old-token"); };
    callbacks.writeToken = [&writtenId, &writtenToken](const QString &id, const QString &token) {
        writtenId = id;
        writtenToken = token;
    };
    callbacks.lastFmConfigured = []() { return false; };

    ScrobbleDestinationsDialog dialog(ScrobbleDestinationConfig::defaults(), nullptr, callbacks);
    dialog.show();
    QCoreApplication::processEvents();

    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table != nullptr);
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toString() == ScrobbleDestinationConfig::listenBrainzId()) {
            table->selectRow(row);
            break;
        }
    }

    QPushButton *edit = nullptr;
    for (QPushButton *candidate : dialog.findChildren<QPushButton *>()) {
        if (candidate->text() == QStringLiteral("Edit…")) {
            edit = candidate;
            break;
        }
    }
    QVERIFY(edit != nullptr);
    QVERIFY(edit->isEnabled());

    QTimer::singleShot(0, []() {
        auto *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (editor == nullptr) {
            return;
        }
        for (QLineEdit *field : editor->findChildren<QLineEdit *>()) {
            if (field->isEnabled()) {
                field->setText(QStringLiteral("replacement-token"));
            }
        }
        editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    QTest::mouseClick(edit, Qt::LeftButton);

    dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(writtenId, ScrobbleDestinationConfig::listenBrainzId());
    QCOMPARE(writtenToken, QStringLiteral("replacement-token"));

    const ScrobbleDestination *official = dialog.destinations().find(ScrobbleDestinationConfig::listenBrainzId());
    QVERIFY(official != nullptr);
    QCOMPARE(official->type, ScrobbleDestination::Type::ListenBrainzCompatible);
    QCOMPARE(official->apiRoot, ListenBrainzUrl::officialApiRoot());
}

QTEST_MAIN(ScrobbleDestinationsDialogTest)
#include "test_scrobble_destinations_dialog.moc"
