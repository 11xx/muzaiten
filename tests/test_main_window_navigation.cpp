#include <sstream>
#define private public
#include "app/AppCore.h"
#undef private
#include "core/Artist.h"
#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "db/SettingsStore.h"
#include "ipc/IpcServer.h"
#include "mpris/MprisService.h"
#include "player/PlayerCore.h"
#include "reco/RadioSession.h"
#include "scrobble/PlayEventRecorder.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/FileExplorerView.h"
#include "ui/MusicExplorerView.h"
#include "ui/PanelSearchController.h"
#include "ui/PlaylistView.h"
#include "ui/SelectionColors.h"
#include "ui/StopAfterDialog.h"
#include "ui/TrackTable.h"

#define private public
#include "ui/MainWindow.h"
#include "ui/PlayerBar.h"
#undef private

#include <QDataStream>
#include <algorithm>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QAction>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QProgressBar>
#include <QLocalSocket>
#include <QMenu>
#include <QMenuBar>
#include <QScopeGuard>
#include <QSet>
#include <QStatusBar>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>

namespace {

bool writeSilentWav(const QString &path)
{
    constexpr quint32 sampleRate = 8000;
    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint32 sampleCount = sampleRate;
    constexpr quint32 dataBytes = sampleCount * channels * (bitsPerSample / 8);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32{36 + dataBytes};
    out.writeRawData("WAVEfmt ", 8);
    out << quint32{16} << quint16{1} << channels << sampleRate;
    out << quint32{sampleRate * channels * (bitsPerSample / 8)};
    out << quint16{channels * (bitsPerSample / 8)} << bitsPerSample;
    out.writeRawData("data", 4);
    out << dataBytes;
    for (quint32 sample = 0; sample < sampleCount; ++sample) {
        out << qint16{0};
    }
    return out.status() == QDataStream::Ok;
}

QStringList trackPaths(const QVector<Track> &tracks)
{
    QStringList paths;
    paths.reserve(tracks.size());
    for (const Track &track : tracks) {
        paths.push_back(track.path);
    }
    return paths;
}

} // namespace

class MainWindowNavigationTest final : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_stateRoot.isValid());
        const QString testStateRoot = m_stateRoot.filePath(QString::fromLatin1(QTest::currentTestFunction()));
        QVERIFY(QDir().mkpath(testStateRoot));
        qputenv("MUZAITEN_STATE_ROOT", testStateRoot.toUtf8());
        qputenv("MUZAITEN_DEMO_SILENT_AUDIO", "1");
    }

    void cleanup()
    {
        qunsetenv("MUZAITEN_STATE_ROOT");
        qunsetenv("MUZAITEN_DEMO_SILENT_AUDIO");
    }

    void constructsAndNavigatesEveryMainView()
    {
        AppCore core;
        MainWindow window(&core);

        QVERIFY(window.m_queueScreen == nullptr);
        QVERIFY(window.m_searchView == nullptr);
        QVERIFY(window.m_libraryFileExplorer == nullptr);
        QVERIFY(window.m_freeRoamFileExplorer == nullptr);
        QVERIFY(window.m_musicExplorerView == nullptr);
        QVERIFY(window.m_playlistView == nullptr);

        window.persistViewState();
        window.switchMainView(MainView::LibraryPanels);
        window.switchMainView(MainView::LibraryMusicExplorer);
        window.switchMainView(MainView::LibraryFileExplorer);
        window.switchMainView(MainView::FreeRoamFileExplorer);
        window.switchMainView(MainView::Search);
        window.switchMainView(MainView::Queue);
        window.switchMainView(MainView::Playlist);
        window.saveAllViewSettings();
        window.resetViewPreferences();
        window.persistViewState();

        QVERIFY(window.m_rootSplitter != nullptr);
        QVERIFY(window.m_libraryFileExplorer != nullptr);
        QVERIFY(window.m_freeRoamFileExplorer != nullptr);
        QVERIFY(window.m_musicExplorerView != nullptr);
        QVERIFY(window.m_searchView != nullptr);
        QVERIFY(window.m_queueScreen != nullptr);
        QVERIFY(window.m_playlistView != nullptr);
    }

    void radioUiStateSurvivesWindowRebuild()
    {
        AppCore core;
        core.setRadioLoading(true);
        core.showWindow();
        QVERIFY(core.window() != nullptr);

        core.player()->setRadioActive(true);
        QVERIFY(core.window()->m_playerBar->m_radioActive);
        QVERIFY(core.window()->m_playerBar->m_radio->isChecked());

        core.releaseWindow();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QVERIFY(core.window() == nullptr);

        core.showWindow();
        QVERIFY(core.window() != nullptr);
        QVERIFY(core.window()->m_playerBar->m_radioActive);
        QVERIFY(core.window()->m_playerBar->m_radio->isChecked());
        QVERIFY(core.window()->m_playerBar->m_radio->isVisible());
        QVERIFY(core.window()->m_playerBar->m_stopRadioAction->isEnabled());
        QVERIFY(core.window()->m_radioProgress->isVisible());

        core.stopRadio();
        core.releaseWindow();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }

    void failedMixPreservesActiveRadioRefill()
    {
        AppCore core;
        Track current;
        current.path = QStringLiteral("/radio-current.flac");
        core.player()->resetQueue({current}, 0);

        TrackScorer::Candidate candidate;
        candidate.path = QStringLiteral("/unresolvable-radio-candidate.flac");
        candidate.songKey = QStringLiteral("song:unresolvable-radio-candidate");
        candidate.artistFolded = QStringLiteral("radio-artist");
        candidate.albumKey = QStringLiteral("radio-album");
        core.m_radioSession = std::make_unique<RadioSession>(
            QVector<TrackScorer::Candidate>{candidate}, QHash<QString, TrackScorer::Affinity>{},
            QHash<QString, double>{}, 30, QDateTime::currentSecsSinceEpoch());
        core.m_radioSessionKind = QStringLiteral("seeded");
        core.player()->setRadioActive(true);

        core.appendRadioBatch(1);
        QVERIFY(core.m_radioTopUpInProgress);
        QVERIFY(!core.startMix(QStringLiteral("rediscovery")));
        QVERIFY(core.player()->radioActive());
        QVERIFY(core.m_radioSession != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(!core.m_radioTopUpInProgress, 10000);
        QVERIFY(core.player()->radioActive());
        QVERIFY(core.m_radioSession != nullptr);
        QCOMPARE(core.player()->queue().first().path, current.path);
        core.stopRadio();
    }

    void failedMixDoesNotCancelPendingSeededStart()
    {
        QTemporaryDir libraryRoot;
        QVERIFY(libraryRoot.isValid());

        Track seed;
        seed.path = libraryRoot.filePath(QStringLiteral("seed.wav"));
        seed.parentDir = libraryRoot.path();
        seed.filename = QStringLiteral("seed.wav");
        seed.title = QStringLiteral("Seed");
        seed.artistName = QStringLiteral("Seed Artist");
        seed.albumArtistName = seed.artistName;
        seed.albumTitle = QStringLiteral("Seed Album");
        seed.durationMs = 1000;
        seed.fileSize = 1;
        seed.fileMtime = 1;
        seed.codec = QStringLiteral("wav");
        QVERIFY(writeSilentWav(seed.path));

        AppCore core;
        QVERIFY2(core.database()->upsertTrack(seed), qPrintable(core.database()->lastError()));
        core.database()->setSetting(QStringLiteral("radio.batchSize"), QStringLiteral("1"));
        QVERIFY(core.startRadio(seed.path));
        QVERIFY(core.player()->radioActive());
        QVERIFY(!core.startMix(QStringLiteral("rediscovery")));

        QTRY_VERIFY_WITH_TIMEOUT(core.m_radioSession != nullptr, 10000);
        QVERIFY(core.player()->radioActive());
        QCOMPARE(core.player()->queue().first().path, seed.path);
        core.stopRadio();
        core.player()->stop();
    }

    void failedRadioStartsDoNotCreateSnapshots()
    {
        AppCore core;
        MainWindow window(&core);
        Track current;
        current.path = QStringLiteral("/failed-radio-start.flac");
        window.m_player->resetQueue({current}, 0);

        window.startMix(QStringLiteral("rediscovery"));
        window.startArtistRadio(QStringLiteral("Missing Artist"));

        QCOMPARE(window.m_player->queue().size(), 1);
        QCOMPARE(window.m_player->queue().first().path, current.path);
        QVERIFY(window.m_queueId.isEmpty());
        QCOMPARE(window.loadQueueSnapshotsRoot().value(QStringLiteral("radioBacklog")).toArray().size(), 0);
        QVERIFY(!core.player()->radioActive());
    }

    void repairsMissingRestoredFreeRoamDirectory()
    {
        QTemporaryDir browsingRoot;
        QVERIFY(browsingRoot.isValid());
        const QString missing = browsingRoot.filePath(QStringLiteral("gone"));

        AppCore core;
        QJsonObject stored{
            {QStringLiteral("freeRoamDirectory"), missing},
            {QStringLiteral("unrelated"), QStringLiteral("preserved")},
        };
        core.settings()->setSetting(QStringLiteral("mainWindow.view"),
                                    QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));

        MainWindow window(&core);
        const QString expectedHome = MainWindow::browsableDirectoryPath(QDir::homePath());
        const QString expected = expectedHome.isEmpty()
            ? MainWindow::browsableDirectoryPath(QDir::rootPath())
            : expectedHome;
        QCOMPARE(window.m_freeRoamDirectory, expected);

        const QJsonObject repaired = QJsonDocument::fromJson(
            core.settings()->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
        QCOMPARE(repaired.value(QStringLiteral("freeRoamDirectory")).toString(), expected);
        QCOMPARE(repaired.value(QStringLiteral("unrelated")).toString(), QStringLiteral("preserved"));
    }

    void repairsToRootWhenHomeIsNotBrowsable()
    {
        QTemporaryDir browsingRoot;
        QVERIFY(browsingRoot.isValid());
        const QString missingHome = browsingRoot.filePath(QStringLiteral("missing-home"));
        const QString stale = browsingRoot.filePath(QStringLiteral("stale"));

        const bool hadHome = qEnvironmentVariableIsSet("HOME");
        const QByteArray oldHome = qgetenv("HOME");
        const auto restoreHome = qScopeGuard([hadHome, oldHome] {
            if (hadHome) {
                qputenv("HOME", oldHome);
            } else {
                qunsetenv("HOME");
            }
        });
        qputenv("HOME", missingHome.toUtf8());
        QCOMPARE(QDir::homePath(), missingHome);

        AppCore core;
        QJsonObject stored{
            {QStringLiteral("freeRoamDirectory"), stale},
            {QStringLiteral("unrelated"), QStringLiteral("preserved")},
        };
        core.settings()->setSetting(QStringLiteral("mainWindow.view"),
                                    QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));

        MainWindow window(&core);
        const QString expected = MainWindow::browsableDirectoryPath(QDir::rootPath());
        QVERIFY(!expected.isEmpty());
        QCOMPARE(window.m_freeRoamDirectory, expected);
        const QJsonObject repaired = QJsonDocument::fromJson(
            core.settings()->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
        QCOMPARE(repaired.value(QStringLiteral("freeRoamDirectory")).toString(), expected);
        QCOMPARE(repaired.value(QStringLiteral("unrelated")).toString(), QStringLiteral("preserved"));
    }

    void invalidFreeRoamRequestsPreserveLocationAndState()
    {
        QTemporaryDir browsingRoot;
        QVERIFY(browsingRoot.isValid());
        const QString valid = QDir::cleanPath(browsingRoot.path());
        QFile file(browsingRoot.filePath(QStringLiteral("track.flac")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        AppCore core;
        MainWindow window(&core);
        window.setFreeRoamDirectory(valid);
        FileExplorerView *explorer = window.ensureFreeRoamFileExplorer();
        const QString stored = core.settings()->setting(QStringLiteral("mainWindow.view"));

        const QStringList invalid{
            QString(),
            browsingRoot.filePath(QStringLiteral("missing")),
            file.fileName(),
        };
        for (const QString &path : invalid) {
            window.setFreeRoamDirectory(path);
            QCOMPARE(window.m_freeRoamDirectory, valid);
            QCOMPARE(explorer->currentDirectory(), valid);
            QCOMPARE(core.settings()->setting(QStringLiteral("mainWindow.view")), stored);
        }
    }

    void unreadableFreeRoamRequestPreservesLocationAndState()
    {
        QTemporaryDir browsingRoot;
        QVERIFY(browsingRoot.isValid());
        const QString unreadable = browsingRoot.filePath(QStringLiteral("unreadable"));
        QVERIFY(QDir().mkdir(unreadable));
        QVERIFY(QFile::setPermissions(unreadable, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QVERIFY(QFile::setPermissions(unreadable, QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        if (QDir(unreadable).isReadable()) {
            QSKIP("QDir reports the permission-restricted fixture as readable in this environment");
        }

        AppCore core;
        QJsonObject stored{
            {QStringLiteral("freeRoamDirectory"), unreadable},
            {QStringLiteral("unrelated"), QStringLiteral("preserved")},
        };
        core.settings()->setSetting(QStringLiteral("mainWindow.view"),
                                    QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));

        MainWindow window(&core);
        const QString expectedHome = MainWindow::browsableDirectoryPath(QDir::homePath());
        const QString expected = expectedHome.isEmpty()
            ? MainWindow::browsableDirectoryPath(QDir::rootPath())
            : expectedHome;
        QCOMPARE(window.m_freeRoamDirectory, expected);
        const QJsonObject repaired = QJsonDocument::fromJson(
            core.settings()->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
        QCOMPARE(repaired.value(QStringLiteral("freeRoamDirectory")).toString(), expected);
        QCOMPARE(repaired.value(QStringLiteral("unrelated")).toString(), QStringLiteral("preserved"));

        window.setFreeRoamDirectory(browsingRoot.path());
        const QString current = window.m_freeRoamDirectory;
        const QString storedSettings = core.settings()->setting(QStringLiteral("mainWindow.view"));
        window.setFreeRoamDirectory(unreadable);
        QCOMPARE(window.m_freeRoamDirectory, current);
        QCOMPARE(core.settings()->setting(QStringLiteral("mainWindow.view")), storedSettings);
    }

    void fileExplorerUpButtonIsAccessibleAndNavigates()
    {
        QTemporaryDir browsingRoot;
        QVERIFY(browsingRoot.isValid());
        const QString child = browsingRoot.filePath(QStringLiteral("child"));
        QVERIFY(QDir().mkdir(child));

        AppCore core;
        MainWindow window(&core);
        window.setFreeRoamDirectory(child);
        FileExplorerView *explorer = window.ensureFreeRoamFileExplorer();
        auto *up = explorer->findChild<QToolButton *>(QStringLiteral("FileExplorerUpButton"));
        QVERIFY(up != nullptr);
        QCOMPARE(up->text(), QString());
        QCOMPARE(up->focusPolicy(), Qt::NoFocus);
        QCOMPARE(up->toolTip(), QStringLiteral("Go up one directory"));
        QCOMPARE(up->accessibleName(), QStringLiteral("Go up one directory"));
        QVERIFY(!up->icon().isNull());

        QTest::mouseClick(up, Qt::LeftButton);
        QCOMPARE(explorer->currentDirectory(), QDir::cleanPath(browsingRoot.path()));
    }

    void playerBarGraysVolumeWhenControlIsDisabled()
    {
        PlayerBar bar;
        QVERIFY(bar.m_volumeButton != nullptr);
        QVERIFY(bar.m_volumeButton->isEnabled());
        QCOMPARE(bar.m_volumeButton->toolTip(), QStringLiteral("Volume"));

        bar.setVolumeControlEnabled(false);
        QVERIFY(!bar.m_volumeButton->isEnabled());
        QCOMPARE(bar.m_volumeButton->toolTip(), QStringLiteral("Volume disabled by the output profile"));

        bar.setVolumeControlEnabled(true);
        QVERIFY(bar.m_volumeButton->isEnabled());
        QCOMPARE(bar.m_volumeButton->toolTip(), QStringLiteral("Volume"));
    }

    void stopAfterDialogUsesExactRangesAndModes()
    {
        StopAfterDialog dialog;
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Stop after"));
        auto *condition = dialog.findChild<QComboBox *>(QStringLiteral("StopAfterCondition"));
        auto *value = dialog.findChild<QSpinBox *>(QStringLiteral("StopAfterValue"));
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(condition != nullptr);
        QVERIFY(value != nullptr);
        QVERIFY(buttons != nullptr);
        QCOMPARE(condition->count(), 2);
        QCOMPARE(condition->itemText(0), QStringLiteral("Minutes"));
        QCOMPARE(condition->itemText(1), QStringLiteral("Songs"));
        QCOMPARE(value->minimum(), 1);
        QCOMPARE(value->maximum(), 1440);
        QCOMPARE(value->value(), 60);
        QVERIFY(buttons->button(QDialogButtonBox::Ok) != nullptr);
        QVERIFY(buttons->button(QDialogButtonBox::Cancel) != nullptr);
        QCOMPARE(dialog.mode(), StopAfterDialog::Mode::Minutes);

        condition->setCurrentIndex(1);
        QCOMPARE(value->minimum(), 1);
        QCOMPARE(value->maximum(), 999);
        QCOMPARE(value->value(), 5);
        QCOMPARE(dialog.mode(), StopAfterDialog::Mode::Songs);

        condition->setCurrentIndex(0);
        QCOMPARE(value->minimum(), 1);
        QCOMPARE(value->maximum(), 1440);
        QCOMPARE(value->value(), 60);
        QCOMPARE(dialog.mode(), StopAfterDialog::Mode::Minutes);
    }

    void playerBarStopAfterMenuAndIndicatorUseSettledUi()
    {
        PlayerBar bar;
        QMenu *playbackMenu = nullptr;
        for (QAction *action : bar.m_menuBar->actions()) {
            if (action->text() == QStringLiteral("Playback")) {
                playbackMenu = action->menu();
                break;
            }
        }
        QVERIFY(playbackMenu != nullptr);
        QMenu *stopAfterMenu = nullptr;
        for (QAction *action : playbackMenu->actions()) {
            if (action->text() == QStringLiteral("Stop after")) {
                stopAfterMenu = action->menu();
                break;
            }
        }
        QVERIFY(stopAfterMenu != nullptr);

        QStringList labels;
        for (QAction *action : stopAfterMenu->actions()) {
            labels.append(action->isSeparator() ? QStringLiteral("<separator>") : action->text());
        }
        QCOMPARE(labels, (QStringList{
            QStringLiteral("15 minutes"),
            QStringLiteral("30 minutes"),
            QStringLiteral("1 hour"),
            QStringLiteral("2 hours"),
            QStringLiteral("<separator>"),
            QStringLiteral("Current song"),
            QStringLiteral("3 songs"),
            QStringLiteral("5 songs"),
            QStringLiteral("10 songs"),
            QStringLiteral("<separator>"),
            QStringLiteral("Custom…"),
            QStringLiteral("<separator>"),
            QStringLiteral("Cancel Stop after"),
        }));
        QCOMPARE(bar.m_stopAfterCancelAction, stopAfterMenu->actions().last());
        QVERIFY(!stopAfterMenu->actions().last()->isVisible());
        QCOMPARE(bar.m_stopAfterIndicator->toolButtonStyle(), Qt::ToolButtonTextOnly);

        QLayout *rootLayout = bar.layout();
        QVERIFY(rootLayout != nullptr);
        QLayout *controlsLayout = rootLayout->itemAt(1)->layout();
        QVERIFY(controlsLayout != nullptr);
        const int indicatorIndex = controlsLayout->indexOf(bar.m_stopAfterIndicator);
        const int volumeIndex = controlsLayout->indexOf(bar.m_volumeButton);
        QVERIFY(indicatorIndex >= 1);
        QCOMPARE(volumeIndex, indicatorIndex + 1);
        QVERIFY(controlsLayout->itemAt(indicatorIndex - 1)->layout() != nullptr);
        QLayout *progressLayout = controlsLayout->itemAt(indicatorIndex - 1)->layout();
        QVERIFY(progressLayout->itemAt(1)->layout() != nullptr);
        QCOMPARE(progressLayout->itemAt(1)->layout()->indexOf(bar.m_stopAfterIndicator), -1);
        QCOMPARE(bar.m_elapsed->contextMenuPolicy(), Qt::CustomContextMenu);
        QCOMPARE(bar.m_progress->contextMenuPolicy(), Qt::CustomContextMenu);
        QCOMPARE(bar.m_duration->contextMenuPolicy(), Qt::CustomContextMenu);

        QSignalSpy cancelRequested(&bar, &PlayerBar::stopAfterCancelRequested);
        PlayerCore::StopAfterStatus deadline;
        deadline.mode = PlayerCore::StopAfterMode::Deadline;
        deadline.remainingMs = 1'799'000;
        bar.setStopAfterStatus(deadline);
        QCOMPARE(bar.m_stopAfterIndicator->text(), QStringLiteral("Stop after 29:59"));
        QVERIFY(!bar.m_stopAfterIndicator->isHidden());
        QCOMPARE(bar.m_stopAfterIndicator->toolTip(), QStringLiteral("Cancel Stop after"));
        QVERIFY(bar.m_stopAfterIndicator->accessibleName().isEmpty());
        const int renderedTextWidth = QFontMetrics(bar.m_stopAfterIndicator->font())
            .horizontalAdvance(bar.m_stopAfterIndicator->text());
        QVERIFY(bar.m_stopAfterIndicator->sizeHint().width() > renderedTextWidth);
        QVERIFY(bar.m_stopAfterIndicator->minimumWidth() >= bar.m_stopAfterIndicator->sizeHint().width());
        const int deadlineMinimumWidth = bar.m_stopAfterIndicator->minimumWidth();
        QVERIFY(stopAfterMenu->actions().last()->isVisible());
        bar.m_stopAfterIndicator->click();
        QCOMPARE(cancelRequested.count(), 1);

        PlayerCore::StopAfterStatus currentSong;
        currentSong.mode = PlayerCore::StopAfterMode::NaturalCompletions;
        currentSong.remainingCompletions = 1;
        bar.setStopAfterStatus(currentSong);
        QCOMPARE(bar.m_stopAfterIndicator->text(), QStringLiteral("Stop after current song"));
        QVERIFY(bar.m_stopAfterIndicator->accessibleName().isEmpty());
        QVERIFY(bar.m_stopAfterIndicator->minimumWidth() >= deadlineMinimumWidth);

        currentSong.remainingCompletions = 3;
        bar.setStopAfterStatus(currentSong);
        QCOMPARE(bar.m_stopAfterIndicator->text(), QStringLiteral("Stop after 3 songs"));
        QVERIFY(bar.m_stopAfterIndicator->accessibleName().isEmpty());
        QVERIFY(bar.m_stopAfterIndicator->minimumWidth() >= deadlineMinimumWidth);

        bar.setStopAfterStatus({});
        QVERIFY(bar.m_stopAfterIndicator->isHidden());
        QCOMPARE(bar.m_stopAfterIndicator->minimumWidth(), 0);
        QVERIFY(bar.m_stopAfterIndicator->accessibleName().isEmpty());
        QVERIFY(!stopAfterMenu->actions().last()->isVisible());
    }

    void overlappingProfileTransitionsIgnoreStopAfterTrigger()
    {
        QTemporaryDir audioRoot;
        QVERIFY(audioRoot.isValid());
        QFile audioFile(audioRoot.filePath(QStringLiteral("profile-transition.wav")));
        QVERIFY(audioFile.open(QIODevice::WriteOnly));
        audioFile.close();

        AppCore core;
        MainWindow window(&core);
        Track track;
        track.path = audioFile.fileName();
        window.m_player->resetQueue({track}, 0);

        const QList<QTimer *> existingTimers = window.findChildren<QTimer *>();
        PlaybackProfile next = window.m_playbackProfile;
        next.device = QStringLiteral("hw:stop-after-profile-test");
        const auto scheduleTransition = [&]() {
            window.applyOutputProfile(next, next.device, -1, 0, 0, true, true);
        };
        scheduleTransition();
        scheduleTransition();

        QList<QTimer *> transitionTimers;
        for (QTimer *timer : window.findChildren<QTimer *>()) {
            if (!existingTimers.contains(timer) && timer->interval() == 1000) {
                transitionTimers.append(timer);
            }
        }
        QCOMPARE(transitionTimers.size(), 2);

        emit core.player()->stopAfterTriggered();
        for (QTimer *timer : transitionTimers) {
            for (int tick = 0; tick < 10; ++tick) {
                QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
            }
        }
        QVERIFY(!window.m_playback->hasSource());
    }

    void stopAfterTriggerCancelsDelayedResume()
    {
        QTemporaryDir audioRoot;
        QVERIFY(audioRoot.isValid());
        const QString audioPath = audioRoot.filePath(QStringLiteral("resume-race.wav"));
        QVERIFY(writeSilentWav(audioPath));

        AppCore core;
        MainWindow window(&core);
        Track track;
        track.path = audioPath;
        track.durationMs = 120'000;
        window.m_player->resetQueue({track}, 0);
        QSignalSpy playEvents(core.playEventRecorder(), &PlayEventRecorder::playEventReady);

        window.resumePlaybackAt(0, 5'000, /*playing=*/false, /*settleDelayMs=*/100);
        window.m_playback->stop();
        const QString savedPlaybackState = QStringLiteral("sentinel");
        window.m_state->setSetting(QStringLiteral("playback.state"), savedPlaybackState);
        core.mpris()->setPositionMs(777);
        const QString elapsed = window.m_playerBar->m_elapsed->text();

        emit core.player()->stopAfterTriggered();
        QTest::qWait(150);

        QCOMPARE(core.mpris()->positionUsec(), qlonglong{777'000});
        QCOMPARE(window.m_playerBar->m_elapsed->text(), elapsed);
        QCOMPARE(window.m_state->setting(QStringLiteral("playback.state")), savedPlaybackState);
        core.playEventRecorder()->flushSessionEnd();
        QCOMPARE(playEvents.count(), 0);
    }

    void manualStopRoutesDisarmStopAfter()
    {
        AppCore core;
        MainWindow window(&core);
        const auto arm = [&core]() {
            core.player()->armStopAfterCompletions(3);
            QCOMPARE(core.player()->stopAfterStatus().mode, PlayerCore::StopAfterMode::NaturalCompletions);
        };

        arm();
        emit core.mpris()->stopRequested();
        QCOMPARE(core.player()->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);

        arm();
        emit window.m_playerBar->stopRequested();
        QCOMPARE(core.player()->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);

        arm();
        QLocalSocket socket;
        socket.connectToServer(core.ipc()->serverPath());
        QVERIFY2(socket.waitForConnected(1000), qPrintable(socket.errorString()));
        socket.write(QJsonDocument(QJsonObject{{QStringLiteral("command"), QStringLiteral("stop")}})
                         .toJson(QJsonDocument::Compact) + '\n');
        QVERIFY(socket.waitForBytesWritten(1000));
        QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
        const QJsonObject reply = QJsonDocument::fromJson(socket.readLine()).object();
        QVERIFY(reply.value(QStringLiteral("ok")).toBool());
        QCOMPARE(core.player()->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);

        if (auto *tray = core.findChild<QSystemTrayIcon *>(); tray != nullptr
            && tray->contextMenu() != nullptr) {
            QAction *stop = nullptr;
            for (QAction *action : tray->contextMenu()->actions()) {
                if (action->text() == QStringLiteral("Stop")) {
                    stop = action;
                    break;
                }
            }
            QVERIFY(stop != nullptr);
            arm();
            stop->trigger();
            QCOMPARE(core.player()->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
        }
    }

    void stopAfterDoesNotEnterSavedQueueOrPlaybackJson()
    {
        AppCore core;
        MainWindow window(&core);
        Track track;
        track.path = QStringLiteral("/temporary/stop-after.wav");
        track.title = QStringLiteral("Stop after test");
        window.m_player->resetQueue({track}, 0);
        window.m_player->armStopAfterCompletions(3);
        window.saveQueueState();
        window.savePlaybackState(true);

        const QJsonObject queue = window.queueSnapshotObject(QStringLiteral("saved queue"));
        QVERIFY(!queue.contains(QStringLiteral("stopAfter")));
        const QJsonObject storedQueue = QJsonDocument::fromJson(
            window.m_state->setting(QStringLiteral("queue.state")).toUtf8()).object();
        QVERIFY(!storedQueue.contains(QStringLiteral("stopAfter")));
        const QJsonObject playback = QJsonDocument::fromJson(
            window.m_state->setting(QStringLiteral("playback.state")).toUtf8()).object();
        QVERIFY(!playback.contains(QStringLiteral("stopAfter")));
    }

    void restoredRadioShuffleUsesCurrentTrackContext()
    {
        QTemporaryDir startupRoot;
        QVERIFY(startupRoot.isValid());
        qputenv("MUZAITEN_STATE_ROOT", startupRoot.path().toUtf8());

        auto makeLibraryTrack = [&startupRoot](const QString &filename, const QString &title) {
            Track track;
            track.path = startupRoot.filePath(filename);
            track.parentDir = startupRoot.path();
            track.filename = filename;
            track.title = title;
            track.artistName = title + QStringLiteral(" Artist");
            track.albumArtistName = track.artistName;
            track.albumTitle = QStringLiteral("Test Album");
            track.durationMs = 180'000;
            track.fileSize = 44;
            track.fileMtime = 1;
            track.codec = QStringLiteral("wav");

            MetadataBlob::FullMetadata metadata;
            metadata.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Rock")});
            const MetadataBlob::Encoded encoded = MetadataBlob::encode(metadata);
            track.fullMetadataBlob = encoded.data;
            track.fullMetadataRawSize = encoded.rawSize;

            QFile file(track.path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QByteArray::fromHex(
                    "524946462400000057415645666d7420100000000100010044ac000088580100020010006461746100000000"));
            }
            return track;
        };

        AppCore core;
        const Track current = makeLibraryTrack(QStringLiteral("current.wav"), QStringLiteral("Current"));
        const Track target = makeLibraryTrack(QStringLiteral("target.wav"), QStringLiteral("Target"));
        QVERIFY2(core.database()->upsertTrack(current), qPrintable(core.database()->lastError()));
        QVERIFY2(core.database()->upsertTrack(target), qPrintable(core.database()->lastError()));

        core.settings()->setSetting(QStringLiteral("playback.shuffleMode"), QStringLiteral("radio"));
        core.settings()->setSetting(QStringLiteral("playback.radioShufflePercent"), QStringLiteral("100"));
        QJsonObject queueState;
        queueState.insert(QStringLiteral("tracks"), QJsonArray{QJsonObject{{QStringLiteral("path"), current.path}}});
        queueState.insert(QStringLiteral("index"), 0);
        queueState.insert(QStringLiteral("playNextInsertIndex"), 1);
        core.settings()->setSetting(
            QStringLiteral("queue.state"),
            QString::fromUtf8(QJsonDocument(queueState).toJson(QJsonDocument::Compact)));

        MainWindow window(&core);
        QCOMPARE(core.player()->currentTrack().path, current.path);

        core.player()->next();

        QCOMPARE(core.player()->currentTrack().path, target.path);
        QVERIFY2(core.radioPickReason(target.path).contains(QStringLiteral("genre")),
                 qPrintable(core.radioPickReason(target.path)));
    }

    void radioAnchorModePersistsAndMenuActionsAreExclusive()
    {
        AppCore core;
        core.database()->setSetting(QStringLiteral("radio.anchorMode"), QStringLiteral("invalid"));
        QCOMPARE(core.radioAnchorMode(), QStringLiteral("pinned"));
        core.setRadioAnchorMode(QStringLiteral("drift"));
        QCOMPARE(core.radioAnchorMode(), QStringLiteral("drift"));
        QCOMPARE(core.database()->setting(QStringLiteral("radio.anchorMode")), QStringLiteral("drift"));

        MainWindow window(&core);
        QMenu *radioMenu = nullptr;
        for (QAction *action : window.m_playerBar->m_menuBar->actions()) {
            if (action->text() == QStringLiteral("Radio")) {
                radioMenu = action->menu();
                break;
            }
        }
        QVERIFY(radioMenu != nullptr);
        QMenu *anchorMenu = nullptr;
        QAction *anchorAction = nullptr;
        QAction *explorationAction = nullptr;
        for (QAction *action : radioMenu->actions()) {
            if (action->text() == QStringLiteral("Anchor for new sessions")) {
                anchorAction = action;
                anchorMenu = action->menu();
            } else if (action->text() == QStringLiteral("Exploration…")) {
                explorationAction = action;
            }
        }
        QVERIFY(anchorAction != nullptr);
        QVERIFY(explorationAction != nullptr);
        QCOMPARE(radioMenu->actions().indexOf(anchorAction) + 1,
                 radioMenu->actions().indexOf(explorationAction));
        QVERIFY(anchorMenu != nullptr);
        QCOMPARE(anchorMenu->actions().size(), 2);
        QCOMPARE(anchorMenu->actions().at(0)->text(), QStringLiteral("Stay near the starting song"));
        QCOMPARE(anchorMenu->actions().at(1)->text(), QStringLiteral("Drift with what plays"));
        QVERIFY(anchorMenu->actions().at(0)->isCheckable());
        QVERIFY(anchorMenu->actions().at(1)->isCheckable());

        radioMenu->aboutToShow();
        QVERIFY(!anchorMenu->actions().at(0)->isChecked());
        QVERIFY(anchorMenu->actions().at(1)->isChecked());

        QSignalSpy modeSpy(window.m_playerBar, &PlayerBar::radioAnchorModeChanged);
        anchorMenu->actions().at(0)->trigger();
        QVERIFY(anchorMenu->actions().at(0)->isChecked());
        QVERIFY(!anchorMenu->actions().at(1)->isChecked());
        QCOMPARE(core.radioAnchorMode(), QStringLiteral("pinned"));
        QCOMPARE(modeSpy.count(), 1);
    }

    void radioSelectionFiltersMissingTracksBeforeSnapshot()
    {
        QTemporaryDir libraryRoot;
        QVERIFY(libraryRoot.isValid());
        const QString libraryPath = libraryRoot.filePath(QStringLiteral("library-track.wav"));
        const QString missingPath = libraryRoot.filePath(QStringLiteral("missing-track.wav"));
        QVERIFY(writeSilentWav(libraryPath));

        Track libraryTrack;
        libraryTrack.path = libraryPath;
        libraryTrack.parentDir = libraryRoot.path();
        libraryTrack.filename = QStringLiteral("library-track.wav");
        libraryTrack.title = QStringLiteral("Library track");
        libraryTrack.artistName = QStringLiteral("Library artist");
        libraryTrack.albumArtistName = libraryTrack.artistName;
        libraryTrack.albumTitle = QStringLiteral("Library album");
        libraryTrack.codec = QStringLiteral("wav");
        MetadataBlob::FullMetadata metadata;
        metadata.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Rock")});
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(metadata);
        libraryTrack.fullMetadataBlob = encoded.data;
        libraryTrack.fullMetadataRawSize = encoded.rawSize;

        AppCore core;
        QVERIFY2(core.database()->upsertTrack(libraryTrack), qPrintable(core.database()->lastError()));
        MainWindow window(&core);
        Track sentinel;
        sentinel.path = QStringLiteral("/sentinel.wav");
        window.m_player->resetQueue({sentinel}, 0);
        window.startRadioFromSeeds({missingPath, libraryPath, libraryPath});

        QCOMPARE(window.m_player->queue().first().path, libraryPath);
        QVERIFY(core.player()->radioActive());
        const QJsonArray radioBacklog = window.loadQueueSnapshotsRoot()
            .value(QStringLiteral("radioBacklog")).toArray();
        QCOMPARE(radioBacklog.size(), 1);
        QCOMPARE(radioBacklog.first().toObject().value(QStringLiteral("tracks")).toArray()
                     .first().toObject().value(QStringLiteral("path")).toString(), sentinel.path);

        core.stopRadio();
        window.m_player->resetQueue({sentinel}, 0);
        const int backlogBeforeMissing = window.loadQueueSnapshotsRoot()
            .value(QStringLiteral("radioBacklog")).toArray().size();
        window.startRadioFromSeeds({missingPath, libraryRoot.filePath(QStringLiteral("also-missing.wav"))});
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Start Radio: none of these tracks are in the library"));
        window.startRadioFromSeeds({missingPath});
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Start Radio: that track is not in the library"));
        QCOMPARE(window.m_player->queue().first().path, sentinel.path);
        QVERIFY(!core.player()->radioActive());
        QCOMPARE(window.loadQueueSnapshotsRoot().value(QStringLiteral("radioBacklog")).toArray().size(),
                 backlogBeforeMissing);
    }

    void fileExplorerRadioKeepsResolvableMixedSelection()
    {
        Track unresolved;
        unresolved.path = QStringLiteral("/free-roam/a-unresolved.wav");
        unresolved.filename = QStringLiteral("a-unresolved.wav");
        Track libraryTrack;
        libraryTrack.path = QStringLiteral("/library/b-library.wav");
        libraryTrack.filename = QStringLiteral("b-library.wav");

        FileExplorerView view;
        view.setTrackResolver([libraryTrack](const QString &path) {
            return path == libraryTrack.path ? libraryTrack : Track{};
        });
        view.setLibraryEntries({}, {unresolved, libraryTrack});
        view.resize(640, 260);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto *tree = view.findChild<QTreeWidget *>();
        QVERIFY(tree != nullptr);
        tree->selectAll();

        QSignalSpy startSpy(&view, &FileExplorerView::startRadioRequested);
        bool sawStartRadio = false;
        QTimer::singleShot(0, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            for (QAction *action : menu->actions()) {
                if (action->text() == QStringLiteral("Start Radio (2)")) {
                    sawStartRadio = true;
                    action->trigger();
                    break;
                }
            }
            menu->close();
        });

        const QRect rowRect = tree->visualRect(tree->model()->index(0, 0));
        QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                                          Qt::DirectConnection,
                                          Q_ARG(QPoint, rowRect.center())));
        QVERIFY(sawStartRadio);
        QCOMPARE(startSpy.count(), 1);
        const QVector<Track> tracks = qvariant_cast<QVector<Track>>(startSpy.first().at(0));
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().path, libraryTrack.path);
    }

    void radioQueueLifecycleReconcilesPendingPaths()
    {
        const auto candidate = [](const QString &path, const QString &artist) {
            TrackScorer::Candidate row;
            row.path = path;
            row.songKey = QStringLiteral("song:") + path;
            row.artistFolded = artist;
            row.albumKey = QStringLiteral("album:") + path;
            row.genresFolded = {QStringLiteral("rock")};
            return row;
        };

        AppCore core;
        const QString anchorPath = QStringLiteral("/anchor.wav");
        const QVector<TrackScorer::Candidate> pool{
            candidate(QStringLiteral("/pick-a.wav"), QStringLiteral("artist-a")),
            candidate(QStringLiteral("/pick-b.wav"), QStringLiteral("artist-b")),
            candidate(QStringLiteral("/pick-c.wav"), QStringLiteral("artist-c")),
        };
        const QVector<TrackScorer::Candidate> anchors{
            candidate(anchorPath, QStringLiteral("anchor")),
        };
        auto session = std::make_unique<RadioSession>(
            pool, QHash<QString, TrackScorer::Affinity>{}, QHash<QString, double>{}, anchors,
            RadioSession::ContextMode::MovingContext, 30, 1'000'000'000);
        const QVector<Track> picks = session->nextTracks(3, {}, [](const QString &path) {
            Track track;
            track.path = path;
            return track;
        });
        QCOMPARE(picks.size(), 3);

        QVector<Track> queue{Track{.path = anchorPath}};
        queue += picks;
        Track manual;
        manual.path = QStringLiteral("/manual.wav");
        queue.insert(3, manual);
        core.player()->resetQueue(queue, 0);
        core.m_radioSession = std::move(session);
        const QStringList generatedPaths = trackPaths(picks);
        core.m_radioPickPaths = QSet<QString>(generatedPaths.cbegin(), generatedPaths.cend());
        core.m_radioSessionKind = QStringLiteral("seeded");
        core.m_radioSessionSeedPaths = {anchorPath};
        core.m_radioSessionSeedPath = anchorPath;
        core.m_radioSessionAnchorMode = QStringLiteral("drift");
        core.player()->setRadioActive(true);
        core.reconcileRadioPendingState();

        auto pendingPaths = [&core]() {
            QStringList paths;
            for (const QJsonValue &value : core.m_radioSession->constraintState()
                                                .value(QStringLiteral("pendingPaths")).toArray()) {
                paths.push_back(value.toString());
            }
            return paths;
        };
        const auto orderedGeneratedPaths = [&core]() {
            QStringList paths;
            const int firstPendingRow = std::max(0, core.player()->queueIndex() + 1);
            for (int row = firstPendingRow; row < core.player()->queue().size(); ++row) {
                const QString &path = core.player()->queue().at(row).path;
                if (core.m_radioPickPaths.contains(path)) {
                    paths.push_back(path);
                }
            }
            return paths;
        };
        QCOMPARE(pendingPaths(), orderedGeneratedPaths());

        core.player()->removeRows({2});
        QCOMPARE(pendingPaths(), orderedGeneratedPaths());

        core.player()->moveRows({3}, 1);
        QCOMPARE(pendingPaths(), orderedGeneratedPaths());

        core.player()->resetQueue({queue.at(0), picks.at(2), manual}, 0);
        QCOMPARE(pendingPaths(), orderedGeneratedPaths());
        const QJsonObject saved = QJsonDocument::fromJson(
            core.settings()->setting(QStringLiteral("radio.session.state")).toUtf8()).object();
        QJsonArray expectedPending;
        for (const QString &path : orderedGeneratedPaths()) {
            expectedPending.append(path);
        }
        QCOMPARE(saved.value(QStringLiteral("pendingPaths")).toArray(), expectedPending);
        core.stopRadio();
    }

    void radioRefreshPicksBelowReconcilesPendingState()
    {
        QTemporaryDir libraryRoot;
        QVERIFY(libraryRoot.isValid());
        const auto candidate = [](const QString &path, int index) {
            TrackScorer::Candidate row;
            row.path = path;
            row.songKey = QStringLiteral("song:refresh-") + QString::number(index);
            row.artistFolded = QStringLiteral("refresh-artist-") + QString::number(index);
            row.albumKey = QStringLiteral("album:refresh-") + QString::number(index);
            row.genresFolded = {QStringLiteral("rock")};
            return row;
        };

        AppCore core;
        core.player()->setPathResolver([](const Track &track) { return track.path; });
        QVector<TrackScorer::Candidate> pool;
        pool.reserve(5);
        for (int index = 0; index < 5; ++index) {
            const QString filename = QStringLiteral("refresh-%1.wav").arg(index);
            Track track;
            track.path = libraryRoot.filePath(filename);
            track.parentDir = libraryRoot.path();
            track.filename = filename;
            track.title = filename;
            track.artistName = QStringLiteral("Refresh artist %1").arg(index);
            track.albumArtistName = track.artistName;
            track.albumTitle = QStringLiteral("Refresh album %1").arg(index);
            track.fileSize = 1;
            track.fileMtime = 1;
            track.codec = QStringLiteral("wav");
            QVERIFY(writeSilentWav(track.path));
            QVERIFY2(core.database()->upsertTrack(track), qPrintable(core.database()->lastError()));
            pool.push_back(candidate(track.path, index));
        }

        const QString anchorPath = QStringLiteral("/refresh-anchor.wav");
        const QVector<TrackScorer::Candidate> anchors{
            candidate(anchorPath, 99),
        };
        auto session = std::make_unique<RadioSession>(
            pool, QHash<QString, TrackScorer::Affinity>{}, QHash<QString, double>{}, anchors,
            RadioSession::ContextMode::MovingContext, 30, 1'000'000'000);
        const QVector<Track> picks = session->nextTracks(3, {}, [](const QString &path) {
            Track track;
            track.path = path;
            return track;
        });
        QCOMPARE(picks.size(), 3);

        Track anchor;
        anchor.path = anchorPath;
        Track manual;
        manual.path = QStringLiteral("/refresh-manual.wav");
        core.player()->resetQueue({anchor, picks.at(0), picks.at(1), manual, picks.at(2)}, 0);
        core.m_radioSession = std::move(session);
        const QStringList generatedPaths = trackPaths(picks);
        core.m_radioPickPaths = QSet<QString>(generatedPaths.cbegin(), generatedPaths.cend());
        core.m_radioSessionKind = QStringLiteral("seeded");
        core.m_radioSessionSeedPaths = {anchorPath};
        core.m_radioSessionSeedPath = anchorPath;
        core.m_radioSessionAnchorMode = QStringLiteral("drift");
        core.setRadioBatchSize(1);
        core.player()->setRadioActive(true);
        core.reconcileRadioPendingState();

        const QString keptPath = picks.at(0).path;
        const QString refreshedPath = picks.at(1).path;
        const QString laterRefreshedPath = picks.at(2).path;
        QVERIFY(core.refreshRadioPicksBelow(1));
        QVERIFY(core.m_radioTopUpInProgress);
        QTRY_VERIFY_WITH_TIMEOUT(!core.m_radioTopUpInProgress, 10000);

        QStringList pendingPaths;
        for (const QJsonValue &value : core.m_radioSession->constraintState()
                                             .value(QStringLiteral("pendingPaths")).toArray()) {
            pendingPaths.push_back(value.toString());
        }
        QVERIFY(pendingPaths.contains(keptPath));
        QVERIFY(!pendingPaths.contains(refreshedPath));
        QVERIFY(!pendingPaths.contains(laterRefreshedPath));

        QStringList queuePaths;
        for (const Track &track : core.player()->queue()) {
            queuePaths.push_back(track.path);
        }
        QVERIFY(queuePaths.contains(keptPath));
        QVERIFY(queuePaths.contains(manual.path));
        QVERIFY(!queuePaths.contains(refreshedPath));
        QVERIFY(!queuePaths.contains(laterRefreshedPath));
        core.stopRadio();
    }

    void radioForwardJumpReconcilesPendingAndConfirmsCurrent()
    {
        const auto candidate = [](const QString &path, const QString &artist) {
            TrackScorer::Candidate row;
            row.path = path;
            row.songKey = QStringLiteral("song:") + path;
            row.artistFolded = artist;
            row.albumKey = QStringLiteral("album:") + path;
            row.genresFolded = {QStringLiteral("rock")};
            return row;
        };

        AppCore core;
        core.player()->setPathResolver([](const Track &track) { return track.path; });
        const QString anchorPath = QStringLiteral("/forward-anchor.wav");
        const QVector<TrackScorer::Candidate> pool{
            candidate(QStringLiteral("/forward-a.wav"), QStringLiteral("artist-a")),
            candidate(QStringLiteral("/forward-b.wav"), QStringLiteral("artist-b")),
            candidate(QStringLiteral("/forward-c.wav"), QStringLiteral("artist-c")),
        };
        const QVector<TrackScorer::Candidate> anchors{candidate(anchorPath, QStringLiteral("anchor"))};
        auto session = std::make_unique<RadioSession>(
            pool, QHash<QString, TrackScorer::Affinity>{}, QHash<QString, double>{}, anchors,
            RadioSession::ContextMode::MovingContext, 30, 1'000'000'000);
        const QVector<Track> picks = session->nextTracks(3, {}, [](const QString &path) {
            Track track;
            track.path = path;
            return track;
        });
        QCOMPARE(picks.size(), 3);
        Track anchor;
        anchor.path = anchorPath;
        Track manual;
        manual.path = QStringLiteral("/forward-manual.wav");
        QVector<Track> queue{anchor, picks.at(0), picks.at(1), manual, picks.at(2)};
        core.player()->resetQueue(queue, 0);
        core.m_radioSession = std::move(session);
        const QStringList generatedPaths = trackPaths(picks);
        core.m_radioPickPaths = QSet<QString>(generatedPaths.cbegin(), generatedPaths.cend());
        core.m_radioSessionKind = QStringLiteral("seeded");
        core.m_radioSessionSeedPaths = {anchorPath};
        core.m_radioSessionSeedPath = anchorPath;
        core.m_radioSessionAnchorMode = QStringLiteral("drift");
        core.player()->setRadioActive(true);
        core.reconcileRadioPendingState();

        core.player()->playAt(2, true, true, true);
        const QJsonObject state = core.m_radioSession->constraintState();
        QCOMPARE(state.value(QStringLiteral("pendingPaths")).toArray(), QJsonArray{picks.at(2).path});
        const QJsonArray confirmed = state.value(QStringLiteral("confirmedContext")).toArray();
        QCOMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed.first().toObject().value(QStringLiteral("path")).toString(), picks.at(1).path);
        const QJsonObject saved = QJsonDocument::fromJson(
            core.settings()->setting(QStringLiteral("radio.session.state")).toUtf8()).object();
        QCOMPARE(saved.value(QStringLiteral("pendingPaths")).toArray(), QJsonArray{picks.at(2).path});
        core.stopRadio();
    }

    void radioQueueRevisionRejectsStaleWorkerCompletion()
    {
        QTemporaryDir libraryRoot;
        QVERIFY(libraryRoot.isValid());
        const QString generatedPath = libraryRoot.filePath(QStringLiteral("stale-worker.wav"));
        QVERIFY(writeSilentWav(generatedPath));

        Track generatedTrack;
        generatedTrack.path = generatedPath;
        generatedTrack.parentDir = libraryRoot.path();
        generatedTrack.filename = QStringLiteral("stale-worker.wav");
        generatedTrack.title = QStringLiteral("Stale worker");
        generatedTrack.artistName = QStringLiteral("Stale artist");
        generatedTrack.albumArtistName = generatedTrack.artistName;
        generatedTrack.albumTitle = QStringLiteral("Stale album");
        generatedTrack.fileSize = 1;
        generatedTrack.fileMtime = 1;
        generatedTrack.codec = QStringLiteral("wav");

        AppCore core;
        QVERIFY2(core.database()->upsertTrack(generatedTrack), qPrintable(core.database()->lastError()));
        core.player()->setPathResolver([](const Track &track) { return track.path; });
        TrackScorer::Candidate generated;
        generated.path = generatedPath;
        generated.songKey = QStringLiteral("song:stale-worker");
        generated.artistFolded = QStringLiteral("stale-artist");
        generated.albumKey = QStringLiteral("album:stale-worker");
        generated.genresFolded = {QStringLiteral("rock")};
        auto session = std::make_unique<RadioSession>(
            QVector<TrackScorer::Candidate>{generated}, QHash<QString, TrackScorer::Affinity>{},
            QHash<QString, double>{}, QVector<TrackScorer::Candidate>{},
            RadioSession::ContextMode::MovingContext, 30, 1'000'000'000);
        Track current;
        current.path = QStringLiteral("/stale-current.wav");
        Track manual;
        manual.path = QStringLiteral("/stale-manual.wav");
        core.player()->resetQueue({current}, 0);
        core.m_radioSession = std::move(session);
        core.m_radioSessionKind = QStringLiteral("seeded");
        core.m_radioSessionSeedPath = current.path;
        core.m_radioSessionSeedPaths = {current.path};
        core.m_radioSessionAnchorMode = QStringLiteral("drift");
        core.setRadioBatchSize(1);
        core.player()->setRadioActive(true);
        core.reconcileRadioPendingState();

        const QJsonObject before = core.m_radioSession->constraintState();
        QSignalSpy loading(&core, &AppCore::radioLoadingChanged);
        core.appendRadioBatch(1);
        QVERIFY(core.m_radioTopUpInProgress);
        const quint64 capturedWorkerRevision = core.m_radioQueueRevision;
        core.player()->appendTracks({manual});
        QVERIFY(core.m_radioQueueRevision > capturedWorkerRevision);
        QVERIFY(capturedWorkerRevision != core.m_radioQueueRevision);
        QTRY_VERIFY_WITH_TIMEOUT(!core.m_radioTopUpInProgress
                                     && !loading.isEmpty()
                                     && !loading.last().at(0).toBool(),
                                 10000);

        QStringList queuePaths;
        for (const Track &track : core.player()->queue()) {
            queuePaths.push_back(track.path);
        }
        QVERIFY(!queuePaths.contains(generatedPath));
        QVERIFY(!core.m_radioPickPaths.contains(generatedPath));
        const QJsonObject after = core.m_radioSession->constraintState();
        QCOMPARE(after.value(QStringLiteral("pendingPaths")), before.value(QStringLiteral("pendingPaths")));
        QCOMPARE(after.value(QStringLiteral("usedPaths")), before.value(QStringLiteral("usedPaths")));
        QCOMPARE(after.value(QStringLiteral("generatedPickCount")),
                 before.value(QStringLiteral("generatedPickCount")));
        QVERIFY(core.m_radioSession->reasonComponentsFor(generatedPath).isEmpty());
        QVERIFY(core.m_radioSession->pickReasons().isEmpty());
        core.stopRadio();
    }

    void radioSpeculativeJitInvalidatesInFlightBatch()
    {
        QTemporaryDir libraryRoot;
        QVERIFY(libraryRoot.isValid());
        AppCore core;

        QVector<TrackScorer::Candidate> candidates;
        for (int index = 0; index < 2; ++index) {
            Track track;
            track.path = libraryRoot.filePath(QStringLiteral("jit-race-%1.wav").arg(index));
            track.parentDir = libraryRoot.path();
            track.filename = QFileInfo(track.path).fileName();
            track.title = QStringLiteral("JIT race %1").arg(index);
            track.artistName = QStringLiteral("JIT artist %1").arg(index);
            track.albumArtistName = track.artistName;
            track.albumTitle = QStringLiteral("JIT album %1").arg(index);
            track.fileSize = 1;
            track.fileMtime = 1;
            track.codec = QStringLiteral("wav");
            QVERIFY(writeSilentWav(track.path));
            QVERIFY2(core.database()->upsertTrack(track), qPrintable(core.database()->lastError()));

            TrackScorer::Candidate candidate;
            candidate.path = track.path;
            candidate.songKey = QStringLiteral("song:jit-race-%1").arg(index);
            candidate.artistFolded = QStringLiteral("jit-artist-%1").arg(index);
            candidate.albumKey = QStringLiteral("album:jit-race-%1").arg(index);
            candidate.genresFolded = {QStringLiteral("rock")};
            candidates.push_back(std::move(candidate));
        }

        Track current;
        current.path = QStringLiteral("/jit-current.wav");
        core.player()->resetQueue({current}, 0);
        core.m_radioSession = std::make_unique<RadioSession>(
            candidates, QHash<QString, TrackScorer::Affinity>{}, QHash<QString, double>{},
            QVector<TrackScorer::Candidate>{}, RadioSession::ContextMode::MovingContext,
            30, 1'000'000'000);
        core.m_radioSessionKind = QStringLiteral("seeded");
        core.m_radioSessionSeedPath = current.path;
        core.m_radioSessionSeedPaths = {current.path};
        core.m_radioSessionAnchorMode = QStringLiteral("drift");
        core.setRadioBatchSize(1);
        core.player()->setRadioActive(true);
        core.installRadioProvider(/*markPicksAsRadio=*/true);

        core.appendRadioBatch(1);
        QVERIFY(core.m_radioTopUpInProgress);
        core.player()->setRepeatMode(RepeatMode::All);
        const QSet<QString> preparedPaths = core.m_radioPickPaths;
        QCOMPARE(preparedPaths.size(), 1);

        QTRY_VERIFY_WITH_TIMEOUT(!core.m_radioTopUpInProgress, 10000);
        for (const Track &track : core.player()->queue()) {
            QVERIFY(!preparedPaths.contains(track.path));
        }
        const QJsonArray usedPaths = core.m_radioSession->constraintState()
                                         .value(QStringLiteral("usedPaths")).toArray();
        QVERIFY(usedPaths.contains(QJsonValue(*preparedPaths.cbegin())));
        core.stopRadio();
    }

    void radioBatchDropsGuiResolutionFailure()
    {
        QTemporaryDir libraryRoot;
        QVERIFY(libraryRoot.isValid());
        const QString generatedPath = libraryRoot.filePath(QStringLiteral("gui-resolution.wav"));
        QVERIFY(writeSilentWav(generatedPath));

        Track generatedTrack;
        generatedTrack.path = generatedPath;
        generatedTrack.parentDir = libraryRoot.path();
        generatedTrack.filename = QStringLiteral("gui-resolution.wav");
        generatedTrack.title = QStringLiteral("GUI resolution");
        generatedTrack.artistName = QStringLiteral("GUI artist");
        generatedTrack.albumArtistName = generatedTrack.artistName;
        generatedTrack.albumTitle = QStringLiteral("GUI album");
        generatedTrack.fileSize = 1;
        generatedTrack.fileMtime = 1;
        generatedTrack.codec = QStringLiteral("wav");

        AppCore core;
        QVERIFY2(core.database()->upsertTrack(generatedTrack), qPrintable(core.database()->lastError()));
        TrackScorer::Candidate generated;
        generated.path = generatedPath;
        generated.songKey = QStringLiteral("song:gui-resolution");
        generated.artistFolded = QStringLiteral("gui-artist");
        generated.albumKey = QStringLiteral("album:gui-resolution");
        generated.genresFolded = {QStringLiteral("rock")};
        auto session = std::make_unique<RadioSession>(
            QVector<TrackScorer::Candidate>{generated}, QHash<QString, TrackScorer::Affinity>{},
            QHash<QString, double>{}, QVector<TrackScorer::Candidate>{},
            RadioSession::ContextMode::MovingContext, 30, 1'000'000'000);
        Track current;
        current.path = QStringLiteral("/gui-resolution-current.wav");
        core.player()->resetQueue({current}, 0);
        core.m_radioSession = std::move(session);
        core.m_radioSessionKind = QStringLiteral("seeded");
        core.m_radioSessionSeedPath = current.path;
        core.m_radioSessionSeedPaths = {current.path};
        core.m_radioSessionAnchorMode = QStringLiteral("drift");
        core.setRadioBatchSize(1);
        core.player()->setRadioActive(true);
        core.reconcileRadioPendingState();

        core.appendRadioBatch(1);
        QVERIFY(core.m_radioTopUpInProgress);
        QCOMPARE(core.database()->markTracksMissing({generatedPath}), 1);
        QCOMPARE(core.database()->removeMissingTracks(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!core.m_radioTopUpInProgress, 10000);

        QStringList queuePaths;
        for (const Track &track : core.player()->queue()) {
            queuePaths.push_back(track.path);
        }
        QVERIFY(!queuePaths.contains(generatedPath));
        QVERIFY(!core.m_radioPickPaths.contains(generatedPath));
        const QJsonObject state = core.m_radioSession->constraintState();
        QCOMPARE(state.value(QStringLiteral("pendingPaths")).toArray(), QJsonArray{});
        core.stopRadio();
    }

    void radioPickRestoreUsesExplicitIdentity()
    {
        AppCore core;
        Track anchor;
        anchor.path = QStringLiteral("/restore-anchor.wav");
        anchor.artistName = QStringLiteral("Anchor");
        anchor.parentDir = QStringLiteral("/");
        anchor.filename = QStringLiteral("restore-anchor.wav");
        anchor.fileSize = 1;
        anchor.fileMtime = 1;
        anchor.albumArtistName = anchor.artistName;
        anchor.albumTitle = QStringLiteral("Anchor album");
        anchor.title = QStringLiteral("Anchor");
        MetadataBlob::FullMetadata anchorMetadata;
        anchorMetadata.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Rock")});
        anchor.fullMetadataBlob = MetadataBlob::encode(anchorMetadata).data;
        Track manual = anchor;
        manual.path = QStringLiteral("/manual-interruption.wav");
        manual.artistName = QStringLiteral("Manual");
        manual.albumArtistName = manual.artistName;
        manual.title = QStringLiteral("Manual");
        Track pick = anchor;
        pick.path = QStringLiteral("/generated-pick.wav");
        pick.artistName = QStringLiteral("Generated");
        pick.albumArtistName = pick.artistName;
        pick.title = QStringLiteral("Generated");
        for (const Track &track : {anchor, manual, pick}) {
            QVERIFY2(core.database()->upsertTrack(track), qPrintable(core.database()->lastError()));
        }
        core.player()->resetQueue({anchor, manual, pick}, 0);

        const auto stateWith = [](bool explicitIdentity, bool pendingIdentity) {
            QJsonObject state{
                {QStringLiteral("active"), true},
                {QStringLiteral("kind"), QStringLiteral("seeded")},
                {QStringLiteral("seedPath"), QStringLiteral("/restore-anchor.wav")},
                {QStringLiteral("seedPaths"), QJsonArray{QStringLiteral("/restore-anchor.wav")}},
                {QStringLiteral("anchorMode"), QStringLiteral("drift")},
                {QStringLiteral("exploration"), 30},
                {QStringLiteral("usedPaths"), QJsonArray{
                    QStringLiteral("/manual-interruption.wav"), QStringLiteral("/generated-pick.wav")}},
            };
            if (explicitIdentity) {
                state.insert(QStringLiteral("radioPickPaths"), QJsonArray{
                    QStringLiteral("/restore-anchor.wav"), QStringLiteral("/generated-pick.wav")});
            }
            if (pendingIdentity) {
                state.insert(QStringLiteral("pendingPaths"), QJsonArray{
                    explicitIdentity ? QStringLiteral("/manual-interruption.wav")
                                     : QStringLiteral("/generated-pick.wav")});
            }
            return state;
        };
        const auto restore = [&core, &anchor, &manual, &pick](const QJsonObject &state,
                                                                 const QSet<QString> &expectedPaths) {
            core.stopRadio();
            core.player()->setRadioActive(false);
            core.player()->resetQueue({anchor, manual, pick}, 0);
            core.settings()->setSetting(QStringLiteral("radio.session.state"),
                                        QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)));
            core.m_radioRestoreDone = false;
            core.maybeRestoreRadioSession();
            QCOMPARE(core.m_radioPickPaths, expectedPaths);
        };

        restore(stateWith(true, true), QSet<QString>{pick.path});
        restore(stateWith(false, true), QSet<QString>{pick.path});
        restore(stateWith(false, false), QSet<QString>{manual.path, pick.path});
        core.stopRadio();
    }

    void movingMultiAnchorStartPersistsAndRestoresOrderedState()
    {
        QTemporaryDir libraryRoot;
        QVERIFY(libraryRoot.isValid());
        auto makeLibraryTrack = [&libraryRoot](const QString &filename, const QString &artist,
                                                const QString &genre) {
            Track track;
            track.path = libraryRoot.filePath(filename);
            track.parentDir = libraryRoot.path();
            track.filename = filename;
            track.title = filename;
            track.artistName = artist;
            track.albumArtistName = artist;
            track.albumTitle = QStringLiteral("Radio album");
            track.durationMs = 180'000;
            track.fileSize = 44;
            track.fileMtime = 1;
            track.codec = QStringLiteral("wav");
            MetadataBlob::FullMetadata metadata;
            metadata.tags.insert(QStringLiteral("GENRE"), {genre});
            const MetadataBlob::Encoded encoded = MetadataBlob::encode(metadata);
            track.fullMetadataBlob = encoded.data;
            track.fullMetadataRawSize = encoded.rawSize;
            return track;
        };

        const Track first = makeLibraryTrack(QStringLiteral("first.wav"), QStringLiteral("First Artist"),
                                             QStringLiteral("Rock"));
        const Track second = makeLibraryTrack(QStringLiteral("second.wav"), QStringLiteral("Second Artist"),
                                              QStringLiteral("Jazz"));
        const Track rockCandidate = makeLibraryTrack(QStringLiteral("candidate-rock.wav"),
                                                     QStringLiteral("Rock Candidate"), QStringLiteral("Rock"));
        const Track jazzCandidate = makeLibraryTrack(QStringLiteral("candidate-jazz.wav"),
                                                     QStringLiteral("Jazz Candidate"), QStringLiteral("Jazz"));
        QVERIFY(writeSilentWav(first.path));
        QVERIFY(writeSilentWav(second.path));
        QVERIFY(writeSilentWav(rockCandidate.path));
        QVERIFY(writeSilentWav(jazzCandidate.path));
        const QString missing = libraryRoot.filePath(QStringLiteral("missing.wav"));

        {
            AppCore core;
            QVERIFY2(core.database()->upsertTrack(first), qPrintable(core.database()->lastError()));
            QVERIFY2(core.database()->upsertTrack(second), qPrintable(core.database()->lastError()));
            QVERIFY2(core.database()->upsertTrack(rockCandidate), qPrintable(core.database()->lastError()));
            QVERIFY2(core.database()->upsertTrack(jazzCandidate), qPrintable(core.database()->lastError()));
            core.database()->setSetting(QStringLiteral("radio.batchSize"), QStringLiteral("3"));
            core.setRadioAnchorMode(QStringLiteral("drift"));
            MainWindow window(&core);

            Track sentinel;
            sentinel.path = QStringLiteral("/sentinel.wav");
            core.player()->resetQueue({sentinel}, 0);
            QVERIFY(!core.startRadio(QStringList{first.path, missing, second.path}));
            QCOMPARE(core.player()->queue().size(), 1);
            QCOMPARE(core.player()->queue().first().path, sentinel.path);
            QVERIFY(!core.player()->radioActive());

            QSignalSpy loading(&core, &AppCore::radioLoadingChanged);
            QVERIFY(core.startRadio(QStringList{QString(), first.path, second.path, first.path}));
            QTRY_VERIFY_WITH_TIMEOUT(!loading.isEmpty() && !loading.last().at(0).toBool(), 10000);

            const QJsonObject state = QJsonDocument::fromJson(
                core.settings()->setting(QStringLiteral("radio.session.state")).toUtf8()).object();
            QCOMPARE(state.value(QStringLiteral("kind")).toString(), QStringLiteral("seeded"));
            QCOMPARE(state.value(QStringLiteral("seedPath")).toString(), first.path);
            QCOMPARE(state.value(QStringLiteral("anchorMode")).toString(), QStringLiteral("drift"));
            const QJsonArray savedSeeds = state.value(QStringLiteral("seedPaths")).toArray();
            QCOMPARE(savedSeeds, (QJsonArray{first.path, second.path}));
            QCOMPARE(core.player()->queue().first().path, first.path);
            QVERIFY(!std::any_of(core.player()->queue().cbegin() + 1, core.player()->queue().cend(),
                                 [&second](const Track &track) { return track.path == second.path; }));
            QStringList queuedPaths;
            for (const Track &track : core.player()->queue()) {
                queuedPaths.push_back(track.path);
            }
            QVERIFY(queuedPaths.contains(rockCandidate.path));
            QVERIFY(queuedPaths.contains(jazzCandidate.path));
            window.saveQueueState();
            core.player()->setRadioActive(false);
            core.player()->setRadioProvider({});
            core.player()->stop();
        }

        AppCore restored;
        restored.showWindow();
        QTRY_VERIFY_WITH_TIMEOUT(restored.player()->radioActive(), 5000);
        QCOMPARE(restored.player()->queue().first().path, first.path);
        const QJsonObject restoredState = QJsonDocument::fromJson(
            restored.settings()->setting(QStringLiteral("radio.session.state")).toUtf8()).object();
        QCOMPARE(restoredState.value(QStringLiteral("anchorMode")).toString(), QStringLiteral("drift"));
        QCOMPARE(restoredState.value(QStringLiteral("seedPaths")).toArray(), (QJsonArray{first.path, second.path}));
        restored.player()->setRadioActive(false);
        restored.player()->setRadioProvider({});
        restored.player()->stop();
        restored.releaseWindow();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }

    void trackTableRadioMenuEmitsSelectedTracksInRowOrder()
    {
        TrackTable table;
        Track first;
        first.path = QStringLiteral("/first.flac");
        Track second;
        second.path = QStringLiteral("/second.flac");
        Track third;
        third.path = QStringLiteral("/third.flac");
        table.setTracks({first, second, third});
        table.resize(700, 260);
        table.show();
        QVERIFY(QTest::qWaitForWindowExposed(&table));
        QTableView *view = &table;
        view->selectionModel()->clearSelection();
        for (const int row : {2, 0, 1}) {
            view->selectionModel()->select(view->model()->index(row, 0),
                                           QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        view->selectionModel()->setCurrentIndex(view->model()->index(1, 0), QItemSelectionModel::NoUpdate);

        QSignalSpy startSpy(&table, &TrackTable::startRadioRequested);
        bool sawAction = false;
        QTimer::singleShot(0, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            for (QAction *action : menu->actions()) {
                if (action->text() == QStringLiteral("Start Radio (3)")) {
                    sawAction = true;
                    action->trigger();
                    break;
                }
            }
            menu->close();
        });

        const QRect rowRect = view->visualRect(view->model()->index(1, 0));
        QVERIFY(QMetaObject::invokeMethod(view, "customContextMenuRequested",
                                          Qt::DirectConnection,
                                          Q_ARG(QPoint, rowRect.center())));
        QVERIFY(sawAction);
        QCOMPARE(startSpy.count(), 1);
        const QVector<Track> tracks = qvariant_cast<QVector<Track>>(startSpy.first().at(0));
        QCOMPARE(tracks.size(), 3);
        QCOMPARE(tracks.at(0).path, first.path);
        QCOMPARE(tracks.at(1).path, second.path);
        QCOMPARE(tracks.at(2).path, third.path);
    }

    void savedQueueLimitsDefaultToFifteenUnlessUnlimited()
    {
        AppCore core;
        MainWindow window(&core);

        QCOMPARE(window.savedQueueLimitSetting(), 15);
        QCOMPARE(window.radioSavedQueueLimitSetting(), 15);
        QVERIFY(!window.savedQueueUnlimitedSetting());
        QVERIFY(!window.radioSavedQueueUnlimitedSetting());

        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("1"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("true"));
        QCOMPARE(window.savedQueueLimitSetting(), 0);
        QCOMPARE(window.radioSavedQueueLimitSetting(), 0);
        QVERIFY(window.savedQueueUnlimitedSetting());
        QVERIFY(window.radioSavedQueueUnlimitedSetting());

        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("0"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("0"));
        QCOMPARE(window.savedQueueLimitSetting(), 15);
        QCOMPARE(window.radioSavedQueueLimitSetting(), 15);
    }

    void radioSnapshotsAreTaggedAndLabeled()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));

        Track track;
        track.path = QStringLiteral("/music/radio-seed.flac");
        track.title = QStringLiteral("Radio Seed");
        track.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({track}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:radio-test"));

        window.snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        const QJsonArray radioBacklog = root.value(QStringLiteral("radioBacklog")).toArray();
        QCOMPARE(root.value(QStringLiteral("backlog")).toArray().size(), 0);
        QCOMPARE(radioBacklog.size(), 1);
        const QJsonObject snapshot = radioBacklog.at(0).toObject();
        QCOMPARE(snapshot.value(QStringLiteral("source")).toString(), QStringLiteral("radio"));
        QCOMPARE(snapshot.value(QStringLiteral("name")).toString(), QString());

        const QString timestamp = QDateTime::fromSecsSinceEpoch(snapshot.value(QStringLiteral("savedAt")).toVariant().toLongLong())
                                      .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"));
        const QVector<SavedQueuePlaylistEntry> entries = window.savedQueuePlaylistEntries();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.at(0).name, QStringLiteral("Radio session %1").arg(timestamp));
    }

    void radioSnapshotsUseSeparateFifteenEntryBucket()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));
        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("0"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("0"));

        for (int i = 0; i < 17; ++i) {
            Track track;
            track.path = QStringLiteral("/music/regular-%1.flac").arg(i);
            track.title = QStringLiteral("Regular %1").arg(i);
            track.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({track}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:regular-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious();
        }
        for (int i = 0; i < 17; ++i) {
            Track track;
            track.path = QStringLiteral("/music/radio-%1.flac").arg(i);
            track.title = QStringLiteral("Radio %1").arg(i);
            track.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({track}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:radio-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));
        }

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        const QJsonArray backlog = root.value(QStringLiteral("backlog")).toArray();
        const QJsonArray radioBacklog = root.value(QStringLiteral("radioBacklog")).toArray();
        QCOMPARE(backlog.size(), 15);
        QCOMPARE(radioBacklog.size(), 15);
        QCOMPARE(backlog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:regular-16"));
        QCOMPARE(backlog.at(14).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:regular-2"));
        QCOMPARE(radioBacklog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:radio-16"));
        QCOMPARE(radioBacklog.at(14).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:radio-2"));
        for (const QJsonValue &value : radioBacklog) {
            QCOMPARE(value.toObject().value(QStringLiteral("source")).toString(), QStringLiteral("radio"));
        }
    }

    void radioAddsUsePlayNextPolicy()
    {
        AppCore core;
        MainWindow window(&core);

        const auto makeTrack = [](const QString &path) {
            Track track;
            track.path = path;
            return track;
        };
        const Track current = makeTrack(QStringLiteral("/music/current.flac"));
        const Track radioPickOne = makeTrack(QStringLiteral("/music/radio-pick-one.flac"));
        const Track radioPickTwo = makeTrack(QStringLiteral("/music/radio-pick-two.flac"));
        const Track firstAddOne = makeTrack(QStringLiteral("/music/first-add-one.flac"));
        const Track firstAddTwo = makeTrack(QStringLiteral("/music/first-add-two.flac"));
        const Track secondAddOne = makeTrack(QStringLiteral("/music/second-add-one.flac"));
        const Track secondAddTwo = makeTrack(QStringLiteral("/music/second-add-two.flac"));
        const Track regularAdd = makeTrack(QStringLiteral("/music/regular-add.flac"));
        const Track savedAddOne = makeTrack(QStringLiteral("/music/saved-add-one.flac"));
        const Track savedAddTwo = makeTrack(QStringLiteral("/music/saved-add-two.flac"));
        const auto queuePaths = [&window] {
            QStringList paths;
            for (const Track &track : window.m_player->queue()) {
                paths.append(track.path);
            }
            return paths;
        };

        window.m_player->resetQueue({savedAddOne, savedAddTwo}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:saved-add"));
        const QJsonObject savedSnapshot = window.queueSnapshotObject(QStringLiteral("saved queue"));
        QJsonObject snapshotRoot = window.loadQueueSnapshotsRoot();
        QJsonArray savedQueues = snapshotRoot.value(QStringLiteral("saved")).toArray();
        savedQueues.append(savedSnapshot);
        snapshotRoot.insert(QStringLiteral("saved"), savedQueues);
        window.saveQueueSnapshotsRoot(snapshotRoot);
        const QString savedId = savedSnapshot.value(QStringLiteral("id")).toString();
        QVERIFY(!savedId.isEmpty());

        window.m_player->resetQueue({current, radioPickOne, radioPickTwo}, 0, 1);
        QCOMPARE(window.m_player->queueIndex(), 0);
        window.m_player->setRadioActive(true);
        QVERIFY(window.m_player->radioActive());

        window.enqueueTracksFromMenu({firstAddOne, firstAddTwo}, QueueAddMode::Append, false);
        QCOMPARE(queuePaths(), (QStringList{
            current.path, firstAddOne.path, firstAddTwo.path, radioPickOne.path, radioPickTwo.path,
        }));
        QCOMPARE(window.m_player->playNextInsertIndex(), 3);

        window.enqueueTracksFromMenu({secondAddOne, secondAddTwo}, QueueAddMode::Append, false);
        QCOMPARE(queuePaths(), (QStringList{
            current.path, firstAddOne.path, firstAddTwo.path,
            secondAddOne.path, secondAddTwo.path, radioPickOne.path, radioPickTwo.path,
        }));
        QCOMPARE(window.m_player->playNextInsertIndex(), 5);

        window.addQueueSnapshotByIdToQueue(savedId);
        QCOMPARE(queuePaths(), (QStringList{
            current.path, firstAddOne.path, firstAddTwo.path,
            secondAddOne.path, secondAddTwo.path, savedAddOne.path, savedAddTwo.path,
            radioPickOne.path, radioPickTwo.path,
        }));
        QCOMPARE(window.m_player->playNextInsertIndex(), 7);

        window.m_player->setRadioActive(false);
        QVERIFY(!window.m_player->radioActive());
        window.enqueueTracksFromMenu({regularAdd}, QueueAddMode::Append, false);
        QCOMPARE(queuePaths(), (QStringList{
            current.path, firstAddOne.path, firstAddTwo.path,
            secondAddOne.path, secondAddTwo.path, savedAddOne.path, savedAddTwo.path,
            radioPickOne.path, radioPickTwo.path, regularAdd.path,
        }));
    }

    void unlimitedSavedQueueTogglesDisableTrimming()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));
        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("1"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("1"));

        for (int i = 0; i < 17; ++i) {
            Track regularTrack;
            regularTrack.path = QStringLiteral("/music/unlimited-regular-%1.flac").arg(i);
            regularTrack.title = QStringLiteral("Unlimited Regular %1").arg(i);
            regularTrack.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({regularTrack}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:unlimited-regular-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious();

            Track radioTrack;
            radioTrack.path = QStringLiteral("/music/unlimited-radio-%1.flac").arg(i);
            radioTrack.title = QStringLiteral("Unlimited Radio %1").arg(i);
            radioTrack.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({radioTrack}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:unlimited-radio-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));
        }

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        QCOMPARE(root.value(QStringLiteral("backlog")).toArray().size(), 17);
        QCOMPARE(root.value(QStringLiteral("radioBacklog")).toArray().size(), 17);
    }

    void legacyRadioSnapshotsMoveOutOfRegularBacklog()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));

        Track radioTrack;
        radioTrack.path = QStringLiteral("/music/legacy-radio.flac");
        radioTrack.title = QStringLiteral("Legacy Radio");
        radioTrack.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({radioTrack}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:legacy-radio"));
        const QJsonObject legacyRadio = window.queueSnapshotObject(QString(), QStringLiteral("radio"));

        Track regularTrack;
        regularTrack.path = QStringLiteral("/music/legacy-regular.flac");
        regularTrack.title = QStringLiteral("Legacy Regular");
        regularTrack.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({regularTrack}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:legacy-regular"));
        const QJsonObject legacyRegular = window.queueSnapshotObject(QString());

        QJsonArray legacyBacklog;
        legacyBacklog.append(legacyRadio);
        legacyBacklog.append(legacyRegular);
        QJsonObject legacyRoot;
        legacyRoot.insert(QStringLiteral("backlog"), legacyBacklog);
        window.saveQueueSnapshotsRoot(legacyRoot);

        Track newRegularTrack;
        newRegularTrack.path = QStringLiteral("/music/new-regular.flac");
        newRegularTrack.title = QStringLiteral("New Regular");
        newRegularTrack.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({newRegularTrack}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:new-regular"));
        window.snapshotCurrentQueueAsPrevious();

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        const QJsonArray backlog = root.value(QStringLiteral("backlog")).toArray();
        const QJsonArray radioBacklog = root.value(QStringLiteral("radioBacklog")).toArray();
        QCOMPARE(backlog.size(), 2);
        QCOMPARE(radioBacklog.size(), 1);
        QCOMPARE(backlog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:new-regular"));
        QCOMPARE(backlog.at(1).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:legacy-regular"));
        QCOMPARE(radioBacklog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:legacy-radio"));
    }

    void legacySnapshotsKeepFallbackLabel()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));

        Track track;
        track.path = QStringLiteral("/music/legacy.flac");
        track.title = QStringLiteral("Legacy");
        track.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({track}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:legacy-test"));

        QJsonObject snapshot = window.queueSnapshotObject(QString());
        snapshot.insert(QStringLiteral("id"), QStringLiteral("queue:legacy-test"));
        snapshot.insert(QStringLiteral("savedAt"), 1'234'567'890);
        QJsonArray backlog;
        backlog.append(snapshot);
        QJsonObject root;
        root.insert(QStringLiteral("backlog"), backlog);
        window.saveQueueSnapshotsRoot(root);

        const QVector<SavedQueuePlaylistEntry> entries = window.savedQueuePlaylistEntries();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.at(0).name, QStringLiteral("saved queue 1"));
    }

    void musicExplorerKeepsMainPanelNavigationActive()
    {
        AppCore core;
        MainWindow window(&core);
        window.resize(900, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.m_artistSidebar->setArtists({
            {.name = QStringLiteral("Artist One"), .albumCount = 1},
            {.name = QStringLiteral("Artist Two"), .albumCount = 1},
        });
        window.switchMainView(MainView::LibraryMusicExplorer);
        QVERIFY(window.m_musicExplorerView != nullptr);

        window.m_panelSearch->setActivePanel(MainPanelId::Artists, true);
        auto *artistList = window.m_artistSidebar->navigationWidget();
        QVERIFY(artistList->hasFocus());
        QCOMPARE(window.m_artistSidebar->currentRow(), 0);

        QTest::keyClick(artistList, Qt::Key_J);
        QCOMPARE(window.m_artistSidebar->currentRow(), 1);

        QTest::keyClick(artistList, Qt::Key_L);
        QCOMPARE(window.m_panelSearch->activePanel(), MainPanelId::Albums);
        QVERIFY(window.m_musicExplorerView->hasFocus());

        QTest::keyClick(window.m_musicExplorerView, Qt::Key_H);
        QCOMPARE(window.m_panelSearch->activePanel(), MainPanelId::Artists);
        QVERIFY(artistList->hasFocus());
    }

    void musicExplorerHCollapsesTracksBeforeMovingPanels()
    {
        AppCore core;
        MainWindow window(&core);
        window.resize(900, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.switchMainView(MainView::LibraryMusicExplorer);
        QVERIFY(window.m_musicExplorerView != nullptr);
        window.m_musicExplorerView->setTrackProvider([](const Album &album) {
            QVector<Track> tracks;
            for (int i = 0; i < 3; ++i) {
                Track track;
                track.path = QStringLiteral("/music/%1/%2.flac").arg(album.title).arg(i + 1);
                track.title = QStringLiteral("Track %1").arg(i + 1);
                track.artistName = QStringLiteral("Artist");
                track.albumArtistName = QStringLiteral("Artist");
                track.albumTitle = album.title;
                track.trackNumber = i + 1;
                tracks.push_back(track);
            }
            return tracks;
        });
        window.m_musicExplorerView->setAlbums({
            {.title = QStringLiteral("One"), .albumArtistName = QStringLiteral("Artist"), .trackCount = 3},
            {.title = QStringLiteral("Two"), .albumArtistName = QStringLiteral("Artist"), .trackCount = 3},
        });

        window.m_panelSearch->setActivePanel(MainPanelId::Albums, true);
        QVERIFY(window.m_musicExplorerView->hasFocus());
        QTest::keyClick(window.m_musicExplorerView, Qt::Key_L);
        QTRY_COMPARE(window.m_panelSearch->activePanel(), MainPanelId::Tracks);
        QTRY_COMPARE(window.m_musicExplorerView->expandedPanelCountForTests(), 1);
        auto *trackTable = window.m_musicExplorerView->trackNavigationWidget();
        QVERIFY(trackTable->hasFocus());

        QTest::keyClick(trackTable, Qt::Key_H);
        QTRY_COMPARE(window.m_panelSearch->activePanel(), MainPanelId::Albums);
        QCOMPARE(window.m_musicExplorerView->expandedPanelCountForTests(), 0);
        QVERIFY(window.m_musicExplorerView->hasFocus());

        QTest::keyClick(window.m_musicExplorerView, Qt::Key_H);
        QCOMPARE(window.m_panelSearch->activePanel(), MainPanelId::Artists);
        QVERIFY(window.m_artistSidebar->navigationWidget()->hasFocus());
    }

    void focusedLibraryAlbumGridUsesActiveSelectionColors()
    {
        AppCore core;
        MainWindow window(&core);
        window.resize(900, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.switchMainView(MainView::LibraryMusicExplorer);
        window.switchMainView(MainView::LibraryPanels);
        QVERIFY(window.m_albumGrid != nullptr);

        window.m_panelSearch->setActivePanel(MainPanelId::Albums, true);
        QTRY_VERIFY(window.m_albumGrid->hasFocus());
        window.m_albumGrid->viewport()->setProperty("mainPanelActive", false);

        QVERIFY(SelectionColors::isActiveMainPanel(window.m_albumGrid->viewport()));
    }

private:
    QTemporaryDir m_stateRoot;
};

QTEST_MAIN(MainWindowNavigationTest)
#include "test_main_window_navigation.moc"
