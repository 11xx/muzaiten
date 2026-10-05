#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QGroupBox>
#include <QLabel>
#include <QTemporaryDir>
#include <QPushButton>
#include <QtTest/QtTest>

#include "AudioDeviceFixtures.h"

#define private public
#include "ui/PlaybackProfileDialog.h"
#undef private

class PlaybackProfileDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The card list consults PipeWire tooling through PATH; with nothing
        // on it, the dialog reads no live audio state.
        QVERIFY(m_emptyPath.isValid());
        qputenv("PATH", m_emptyPath.path().toUtf8());
    }

    // A preferred endpoint that is absent stays selected and is returned
    // unchanged, never swapped for one that happens to be present.
    void absentPreferredEndpointIsKept()
    {
        PlaybackProfileDialog dialog;
        dialog.setSharedEndpoints({{QStringLiteral("desk_dac"), QStringLiteral("Desk DAC")},
                                   {QStringLiteral("virtual_mix"), QStringLiteral("Virtual mix")}},
                                  true);
        PlaybackProfile profile;
        profile.sharedEndpoint = QStringLiteral("travel_dac");
        profile.sharedEndpointLabel = QStringLiteral("Travel DAC");
        profile.sharedEndpointFallback = true;
        profile.deviceId = QStringLiteral("alsa_card.direct");
        dialog.setProfile(profile);

        QCOMPARE(dialog.m_endpointCombo->currentText(), QStringLiteral("Travel DAC (unavailable)"));
        const PlaybackProfile result = dialog.profile();
        QCOMPARE(result.sharedEndpoint, QStringLiteral("travel_dac"));
        QCOMPARE(result.sharedEndpointLabel, QStringLiteral("Travel DAC"));
        QVERIFY(result.sharedEndpointFallback);
        QVERIFY(result.device.isEmpty());

        dialog.m_endpointCombo->setCurrentIndex(dialog.m_endpointCombo->findData(QStringLiteral("virtual_mix")));
        QCOMPARE(dialog.profile().sharedEndpoint, QStringLiteral("virtual_mix"));
        QCOMPARE(dialog.profile().sharedEndpointLabel, QStringLiteral("Virtual mix"));

        dialog.m_endpointCombo->setCurrentIndex(0);
        QVERIFY(dialog.profile().sharedEndpoint.isEmpty());
        QVERIFY(!dialog.m_endpointFallback->isEnabled());
    }

    // The ALSA transport and backends without routing cannot honor a
    // choice, so the control is disabled, while the choice itself is kept.
    void outputChoiceFollowsTransportCapability()
    {
        PlaybackProfileDialog dialog;
        dialog.setSharedEndpoints({{QStringLiteral("desk_dac"), QStringLiteral("Desk DAC")}}, true);
        PlaybackProfile profile;
        profile.sharedEndpoint = QStringLiteral("desk_dac");
        dialog.setProfile(profile);
        QVERIFY(dialog.m_endpointCombo->isEnabled());
        dialog.m_sink->setCurrentIndex(dialog.m_sink->findData(QStringLiteral("alsa")));
        QVERIFY(!dialog.m_endpointCombo->isEnabled());
        QCOMPARE(dialog.profile().sharedEndpoint, QStringLiteral("desk_dac"));

        PlaybackProfileDialog unsupported;
        unsupported.setSharedEndpoints({}, false);
        unsupported.setProfile(PlaybackProfile{});
        QVERIFY(!unsupported.m_endpointCombo->isEnabled());
    }

    // The remembered direct card is absent and another has its number: the
    // dialog lists the remembered one as unavailable and returns its
    // identity unchanged, in either mode, without offering to take over the
    // other card.
    void absentDirectIdentityIsKept()
    {
        FakeAudioGraph graph;
        graph.cards = {{41, 2, QStringLiteral("alsa_card.b"), QStringLiteral("Card B")}};
        for (const QString &mode : {QStringLiteral("bit-perfect"), QStringLiteral("shared")}) {
            PlaybackProfileDialog dialog;
            PlaybackProfile profile;
            profile.mode = mode;
            profile.device = mode == QStringLiteral("bit-perfect") ? QStringLiteral("hw:2") : QString();
            profile.deviceId = QStringLiteral("alsa_card.a");
            profile.deviceLabel = QStringLiteral("Card A");
            dialog.setProfile(profile);

            QCOMPARE(dialog.m_deviceCombo->currentText(), QStringLiteral("Card A (unavailable)"));
            QVERIFY(dialog.m_deviceAction->isHidden());
            const PlaybackProfile result = dialog.profile();
            QCOMPARE(result.deviceId, QStringLiteral("alsa_card.a"));
            QCOMPARE(result.deviceLabel, QStringLiteral("Card A"));
            if (mode == QStringLiteral("bit-perfect")) {
                QCOMPARE(result.device, QStringLiteral("hw:2"));
            }
        }
        QVERIFY(graph.profileSwitches.isEmpty());
    }

    void statusDescribesWaitAndTransport()
    {
        PlaybackProfileDialog dialog;
        OutputStatus status;
        status.route = OutputStatus::Route::Waiting;
        status.transport = QStringLiteral("PulseAudio");
        status.transportNote = QStringLiteral("the GStreamer PipeWire plugin is not installed");
        status.preferredEndpoint = QStringLiteral("desk_dac");
        status.preferredLabel = QStringLiteral("Desk DAC");
        dialog.setOutputStatus(status);
        const QString text = dialog.m_outputStatus->text();
        QVERIFY(text.contains(QStringLiteral("Using PulseAudio (the GStreamer PipeWire plugin is not installed)")));
        QVERIFY(text.contains(QStringLiteral("Waiting for Desk DAC")));
        QVERIFY(!text.contains(QChar(0x2014)));

        status.route = OutputStatus::Route::Fallback;
        status.currentEndpoint = QStringLiteral("board_out");
        status.currentLabel = QStringLiteral("Board output");
        dialog.setOutputStatus(status);
        QVERIFY(dialog.m_outputStatus->text().contains(QStringLiteral("Desk DAC is missing")));
        QVERIFY(dialog.m_outputStatus->text().contains(QStringLiteral("Now playing to Board output")));
    }

private:
    QTemporaryDir m_emptyPath;
};

QTEST_MAIN(PlaybackProfileDialogTest)

#include "test_playback_profile_dialog.moc"
