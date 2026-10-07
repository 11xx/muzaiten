#include "reco/TasteProfile.h"
#include "ui/TasteProfileDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class TasteProfileDialogTest final : public QObject {
    Q_OBJECT
private slots:
    void explicitPreviewAndImport();
};

void TasteProfileDialogTest::explicitPreviewAndImport()
{
    const QString evidence = qEnvironmentVariable("MUZAITEN_TASTE_EVIDENCE");
    for (const QString &key : QProcessEnvironment::systemEnvironment().keys()) {
        if (key.startsWith(QLatin1String("MUZAITEN_"))) {
            qunsetenv(key.toUtf8().constData());
        }
    }
    QTemporaryDir temporary(evidence.isEmpty() ? QDir::tempPath() + QStringLiteral("/taste-ui-XXXXXX")
                                               : evidence + QStringLiteral("/gui-XXXXXX"));
    QVERIFY(temporary.isValid());
    temporary.setAutoRemove(evidence.isEmpty());
    qputenv("MUZAITEN_STATE_ROOT", temporary.path().toUtf8());
    QDir().mkpath(temporary.path() + QStringLiteral("/data"));
    const QString connection = QStringLiteral("taste-ui-fixture");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(temporary.path() + QStringLiteral("/data/library.sqlite"));
        QVERIFY(db.open());
        QSqlQuery query(db);
        for (const QString &sql : {
            QStringLiteral("CREATE TABLE genre_aliases(alias_folded TEXT, canonical_folded TEXT)"),
            QStringLiteral("CREATE TABLE radio_ignored_genres(genre_folded TEXT)"),
            QStringLiteral("CREATE TABLE tracks(id INTEGER, path TEXT, musicbrainz_recording_id TEXT, artist_name TEXT, title TEXT, original_date TEXT, date TEXT, missing INTEGER, metadata_scanned INTEGER)"),
            QStringLiteral("CREATE TABLE track_genres(track_id INTEGER, genre_folded TEXT)")}) {
            QVERIFY(query.exec(sql));
        }
        for (int i = 0; i < 5; ++i) {
            QVERIFY(query.exec(QStringLiteral("INSERT INTO tracks VALUES(%1, 'fixture-%1', '', 'fixture', 'title-%1', '', '1987', 0, 1)").arg(i)));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO track_genres VALUES(%1, 'jazz')").arg(i)));
        }
        TasteProfileDialog dialog;
        QSignalSpy started(&dialog, &TasteProfileDialog::startRequested);
        auto button = [](QWidget *parent, const QString &text) {
            for (auto *candidate : parent->findChildren<QPushButton *>()) {
                if (candidate->text() == text) {
                    return candidate;
                }
            }
            return static_cast<QPushButton *>(nullptr);
        };
        auto *exportButton = button(&dialog, QStringLiteral("Preview library export…"));
        QVERIFY(exportButton != nullptr);
        const QString output = temporary.path() + QStringLiteral("/export.json");
        QByteArray displayed;
        QTimer::singleShot(0, &dialog, [&]() {
            auto *preview = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(preview != nullptr);
            auto *contents = preview->findChild<QPlainTextEdit *>(QStringLiteral("tastePreviewContents"));
            QVERIFY(contents != nullptr);
            QVERIFY(contents->isReadOnly());
            displayed = contents->toPlainText().toUtf8();
            QVERIFY(!QFile::exists(output));
            preview->reject();
        });
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        exportButton->click();
        QVERIFY(!displayed.isEmpty());
        QVERIFY(!QFile::exists(output));
        QTimer::singleShot(0, &dialog, [&]() {
            auto *preview = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(preview != nullptr);
            QCOMPARE(preview->findChild<QPlainTextEdit *>(QStringLiteral("tastePreviewContents"))->toPlainText().toUtf8(), displayed);
            QVERIFY(query.exec(QStringLiteral("UPDATE track_genres SET genre_folded = 'rock'")));
            QTimer::singleShot(0, &dialog, [&]() {
                auto *destination = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                QVERIFY(destination != nullptr);
                destination->setDirectory(temporary.path());
                destination->selectFile(QStringLiteral("export.json"));
                QVERIFY(QMetaObject::invokeMethod(destination, "accept", Qt::DirectConnection));
            });
            button(preview, QStringLiteral("Export these contents…"))->click();
        });
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        exportButton->click();
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), displayed);
        file.close();
        QTimer::singleShot(0, &dialog, [&]() {
            auto *source = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(source != nullptr);
            source->setDirectory(temporary.path());
            source->selectFile(QStringLiteral("export.json"));
            QTimer::singleShot(0, &dialog, [&]() {
                auto *preview = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(preview != nullptr);
                QCOMPARE(preview->findChild<QPlainTextEdit *>(QStringLiteral("tastePreviewContents"))->toPlainText().toUtf8(), displayed);
                QVERIFY(!QFile::exists(TasteProfiles::storagePath()));
                auto *name = preview->findChild<QLineEdit *>();
                QVERIFY(name != nullptr);
                name->setText(QStringLiteral("Synthetic seed"));
                button(preview, QStringLiteral("Import seed"))->click();
            });
            QVERIFY(QMetaObject::invokeMethod(source, "accept", Qt::DirectConnection));
        });
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        button(&dialog, QStringLiteral("Preview import…"))->click();
        QCOMPARE(started.count(), 0);
        auto *seeds = dialog.findChild<QComboBox *>(QStringLiteral("tasteSeeds"));
        QCOMPARE(seeds->currentText(), QStringLiteral("Synthetic seed"));
        button(&dialog, QStringLiteral("Start radio"))->click();
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.at(0).first().toString(), QStringLiteral("Synthetic seed"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

int main(int argc, char **argv)
{
    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    qputenv("QT_QPA_PLATFORMTHEME", "");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    TasteProfileDialogTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_taste_profile_dialog.moc"
