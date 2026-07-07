#include "ui/TrackMenuSections.h"

#include <QAction>
#include <QMenu>
#include <QtTest/QtTest>

class TrackMenuSectionsTest final : public QObject {
    Q_OBJECT

private slots:
    void canonicalOrderSuffixesAndChecks()
    {
        QMenu menu;
        TrackMenuSections::Callbacks callbacks;
        callbacks.playNow = []() {};
        callbacks.playNext = []() {};
        callbacks.addToQueue = []() {};
        callbacks.playNextTemporary = []() {};
        callbacks.addToQueueTemporary = []() {};
        callbacks.addToPlaylist = []() {};
        callbacks.startRadio = []() {};
        callbacks.setNeverRadio = [](bool) {};
        callbacks.setNoLearn = [](bool) {};
        callbacks.findInLibrary = []() {};
        callbacks.openContainingDirectory = []() {};
        callbacks.copyPath = []() {};
        callbacks.properties = []() {};

        TrackMenuSections::appendTrackSections(menu, callbacks, {.trackCount = 3,
                                                                  .neverRadioChecked = true,
                                                                  .noLearnChecked = false});

        QStringList labels;
        for (QAction *action : menu.actions()) {
            labels << (action->isSeparator() ? QStringLiteral("|") : action->text());
        }
        QCOMPARE(labels, (QStringList{
                             QStringLiteral("Play now (3)"),
                             QStringLiteral("Play next (3)"),
                             QStringLiteral("Add to queue (3)"),
                             QStringLiteral("Play next (don't save to playlist) (3)"),
                             QStringLiteral("Add to queue (don't save to playlist) (3)"),
                             QStringLiteral("|"),
                             QStringLiteral("Add to playlist… (3)"),
                             QStringLiteral("|"),
                             QStringLiteral("Start Radio"),
                             QStringLiteral("Never play on radio"),
                             QStringLiteral("Don't learn from this"),
                             QStringLiteral("|"),
                             QStringLiteral("Find in library"),
                             QStringLiteral("Open containing directory"),
                             QStringLiteral("Copy path"),
                             QStringLiteral("Properties"),
                         }));
        QVERIFY(menu.actions().at(9)->isCheckable());
        QVERIFY(menu.actions().at(9)->isChecked());
        QVERIFY(menu.actions().at(10)->isCheckable());
        QVERIFY(!menu.actions().at(10)->isChecked());
    }

    void omitsAbsentCallbacksAndUsesSingleTrackPlayLabel()
    {
        QMenu menu;
        TrackMenuSections::Callbacks callbacks;
        callbacks.playNow = []() {};
        callbacks.copyPath = []() {};

        TrackMenuSections::appendTrackSections(menu, callbacks, {});

        QStringList labels;
        for (QAction *action : menu.actions()) {
            labels << (action->isSeparator() ? QStringLiteral("|") : action->text());
        }
        QCOMPARE(labels, (QStringList{
                             QStringLiteral("Play"),
                             QStringLiteral("|"),
                             QStringLiteral("Copy path"),
                         }));
    }
};

QTEST_MAIN(TrackMenuSectionsTest)

#include "test_track_menu_sections.moc"
