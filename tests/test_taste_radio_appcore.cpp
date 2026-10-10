#include <sstream>
#define private public
#include "app/AppCore.h"
#undef private

#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "db/SettingsStore.h"
#include "playback/PlaybackBackend.h"
#include "player/PlayerCore.h"
#include "reco/RadioSession.h"
#include "reco/TasteProfile.h"
#include "scrobble/ListenHistoryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <tuple>

namespace {

class FakePlayback final : public PlaybackBackend {
public:
    void setProfile(const PlaybackProfile &) override {}
    void play(const QUrl &url) override { m_source = url; m_state = State::Playing; }
    void prepareNext(const QUrl &) override {}
    void pause() override { m_state = State::Paused; }
    void resume() override { m_state = State::Playing; }
    void stop() override { m_source.clear(); m_state = State::Stopped; }
    void seek(qint64) override {}
    void setVolume(double) override {}
    State state() const override { return m_state; }
    bool hasSource() const override { return !m_source.isEmpty(); }
    qint64 position() const override { return 0; }
    qint64 duration() const override { return 180000; }
private:
    QUrl m_source;
    State m_state = State::Stopped;
};

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QJsonObject savedState(AppCore &core)
{
    return QJsonDocument::fromJson(core.settings()->setting(QStringLiteral("radio.session.state")).toUtf8()).object();
}

} // namespace

class TasteRadioAppCoreTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        const auto environment = QProcessEnvironment::systemEnvironment();
        for (const QString &key : environment.keys()) {
            if (key.startsWith(QLatin1String("MUZAITEN_")) && key != QLatin1String("MUZAITEN_TASTE_EVIDENCE")) {
                qunsetenv(key.toUtf8().constData());
            }
        }
    }

    void init()
    {
        const QString evidence = qEnvironmentVariable("MUZAITEN_TASTE_EVIDENCE");
        m_root = std::make_unique<QTemporaryDir>(evidence.isEmpty()
            ? QDir::tempPath() + QStringLiteral("/taste-core-XXXXXX")
            : evidence + QStringLiteral("/core-XXXXXX"));
        QVERIFY(m_root->isValid());
        m_root->setAutoRemove(evidence.isEmpty());
        qputenv("MUZAITEN_STATE_ROOT", m_root->path().toUtf8());
    }

    void startRestoreStop()
    {
        TasteProfile source;
        source.genres = {{QStringLiteral("bebop"), 0.6}, {QStringLiteral("noise"), 0.4}};
        QString error;
        const QString name = QStringLiteral("Named source seed");
        QVector<Track> queue;
        QJsonObject saved;
        {
            AppCore core(new FakePlayback, nullptr);
            QVERIFY(core.directDevices() == nullptr);
            QVERIFY(QDir(m_root->path()).mkpath(QStringLiteral("music")));
            for (int i = 0; i < 16; ++i) {
                Track track;
                track.path = m_root->filePath(QStringLiteral("music/%1.flac").arg(i));
                track.parentDir = m_root->filePath(QStringLiteral("music"));
                track.filename = QFileInfo(track.path).fileName();
                track.title = QStringLiteral("Synthetic track %1").arg(i);
                track.artistName = track.albumArtistName = QStringLiteral("Synthetic artist %1").arg(i);
                track.albumTitle = QStringLiteral("Synthetic album %1").arg(i);
                track.durationMs = 180000;
                track.codec = QStringLiteral("flac");
                MetadataBlob::FullMetadata metadata;
                metadata.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Bebop")});
                const auto encoded = MetadataBlob::encode(metadata);
                track.fullMetadataBlob = encoded.data;
                track.fullMetadataRawSize = encoded.rawSize;
                QVERIFY(writeBytes(track.path, "fake playback fixture\n"));
                QVERIFY2(core.database()->upsertTrack(track), qPrintable(core.database()->lastError()));
            }
            QVERIFY(core.database()->setGenreAlias(QStringLiteral("bebop"), QStringLiteral("rock'n'roll")));
            QVERIFY(core.database()->setGenreAlias(QStringLiteral("noise"), QStringLiteral("noise/drone")));
            core.database()->setSetting(QStringLiteral("radio.exploration"), QStringLiteral("0"));
            core.database()->setSetting(QStringLiteral("radio.batchSize"), QStringLiteral("2"));
            QVERIFY2(TasteProfiles::importProfile(name, source, false, false, &error), qPrintable(error));
            QVERIFY(!core.player()->radioActive());
            QVERIFY(core.handleIpcCommand(QStringLiteral("start-taste-radio"), {}).contains(QStringLiteral("error")));
            QCOMPARE(core.handleIpcCommand(QStringLiteral("start-taste-radio"), {{QStringLiteral("name"), name}})
                         .value(QStringLiteral("radio")).toString(), QStringLiteral("started"));
            QVERIFY(core.player()->radioActive());
            QCOMPARE(core.activeTasteSeedName(), name);
            queue = core.player()->queue();
            QCOMPARE(queue.size(), 2);
            saved = savedState(core);
            QCOMPARE(saved.value(QStringLiteral("kind")).toString(), QStringLiteral("taste"));
            const QJsonObject seed = saved.value(QStringLiteral("tasteSeed")).toObject();
            QCOMPARE(seed.value(QStringLiteral("name")).toString(), name);
            TasteProfile canonical;
            QVERIFY(TasteProfiles::parse(QJsonDocument(seed.value(QStringLiteral("profile")).toObject()).toJson(), &canonical, &error));
            QCOMPARE(canonical.genres, (QMap<QString, double>{{QStringLiteral("rock'n'roll"), 1.0}}));
            for (const Track &track : queue) {
                QVERIFY(core.radioPickReason(track.path).contains(name));
            }
            QVERIFY(writeBytes(m_root->filePath(QStringLiteral("saved-session.json")), QJsonDocument(saved).toJson()));
            QVERIFY(TasteProfiles::remove(name, &error));
            QCOMPARE(core.activeTasteSeedName(), name);
        }
        {
            AppCore core(new FakePlayback, nullptr);
            core.player()->resetQueue(queue, 0);
            core.maybeRestoreRadioSession();
            QVERIFY(core.player()->radioActive());
            QCOMPARE(core.activeTasteSeedName(), name);
            QCOMPARE(core.m_radioSession->tasteProfileBytes(), QJsonDocument(saved.value(QStringLiteral("tasteSeed"))
                .toObject().value(QStringLiteral("profile")).toObject()).toJson());
            const auto picks = core.m_radioSession->nextTracks(1, {}, [&core](const QString &path) {
                return core.database()->trackForPath(path);
            });
            QCOMPARE(picks.size(), 1);
            QVERIFY(core.radioPickReason(picks.first().path).contains(name));
            core.recordRadioPicks(picks);
            const auto history = core.listenHistory()->radioPickEvents();
            QVERIFY(!history.isEmpty());
            for (const auto &event : history) {
                QVERIFY(std::ranges::any_of(event.components, [&name](const auto &component) {
                    return component.name == QStringLiteral("taste seed: %1").arg(name);
                }));
            }
            core.stopRadio();
            QVERIFY(!core.player()->radioActive());
            QVERIFY(core.activeTasteSeedName().isEmpty());
            QVERIFY(core.settings()->setting(QStringLiteral("radio.session.state")).isEmpty());
            QCOMPARE(core.startTasteRadio(name), AppCore::TasteRadioStart::Unavailable);
            QCOMPARE(core.listenHistory()->radioPickEvents().size(), history.size());
        }
    }

    void nearLimitSeedThatGrowsUnderAliases()
    {
        QString error;
        // 256 two-byte-letter genres at exactly representable 1/256 shares, padded
        // with decades until the accepted profile sits just under the bound.
        // Aliasing one genre to a placeholder drops it, and renormalizing 255
        // shares to 1/255 lengthens every remaining number.
        const QString letters = QStringLiteral("αβγδεζηθικλμνξοπρστυφχψω");
        TasteProfile seed;
        for (int i = 0; i < 256; ++i) {
            QString label = QString(letters.at(i / 24)) + letters.at(i % 24);
            label += QString(98, letters.at(0));
            seed.genres.insert(label, 1.0 / 256.0);
        }
        QByteArray accepted;
        for (int decades = 1; decades <= 199; ++decades) {
            TasteProfile candidate = seed;
            for (int i = 0; i < decades; ++i) {
                candidate.decades.insert(QString::number(1000 + 10 * i), 1.0 / decades);
            }
            QByteArray bytes;
            if (!TasteProfiles::encode(candidate, &bytes, &error)) {
                break;
            }
            accepted = bytes;
            seed.decades = candidate.decades;
        }
        QVERIFY2(!accepted.isEmpty(), qPrintable(error));
        const QString dropped = seed.genres.firstKey();
        const TasteProfile canonical = TasteProfiles::withAliases(seed, {{dropped, QStringLiteral("other")}});
        QCOMPARE(canonical.genres.size(), 255);
        QByteArray renormalized;
        QVERIFY(!TasteProfiles::encode(canonical, &renormalized, &error));
        qInfo("accepted %lld bytes; renormalized under the alias: %s",
              static_cast<long long>(accepted.size()), qPrintable(error));
        QVERIFY(accepted.size() <= TasteProfiles::MaximumBytes);
        QVERIFY(!TasteProfiles::exceedsBound(seed));
        QVERIFY(TasteProfiles::exceedsBound(canonical));

        AppCore core(new FakePlayback, nullptr);
        QVERIFY(QDir(m_root->path()).mkpath(QStringLiteral("music")));
        for (int i = 0; i < 4; ++i) {
            Track track;
            track.path = m_root->filePath(QStringLiteral("music/%1.flac").arg(i));
            track.parentDir = m_root->filePath(QStringLiteral("music"));
            track.filename = QFileInfo(track.path).fileName();
            track.title = QStringLiteral("Synthetic track %1").arg(i);
            track.artistName = track.albumArtistName = QStringLiteral("Synthetic artist %1").arg(i);
            track.albumTitle = QStringLiteral("Synthetic album %1").arg(i);
            track.durationMs = 180000;
            track.codec = QStringLiteral("flac");
            QVERIFY(writeBytes(track.path, "fake playback fixture\n"));
            QVERIFY2(core.database()->upsertTrack(track), qPrintable(core.database()->lastError()));
        }
        core.database()->setSetting(QStringLiteral("radio.batchSize"), QStringLiteral("2"));
        const QString name = QStringLiteral("Near the bound");
        QVERIFY2(TasteProfiles::importProfile(name, seed, false, false, &error), qPrintable(error));
        const auto start = [&core, &name] {
            return core.handleIpcCommand(QStringLiteral("start-taste-radio"), {{QStringLiteral("name"), name}})
                .value(QStringLiteral("radio")).toString();
        };
        // Control: without the alias the same seed starts.
        QCOMPARE(start(), QStringLiteral("started"));
        core.stopRadio();
        QVERIFY(!core.player()->radioActive());
        const QVector<Track> queue = core.player()->queue();

        QVERIFY(core.database()->setGenreAlias(dropped, QStringLiteral("other")));
        QCOMPARE(start(), QStringLiteral("taste-seed-too-large"));
        QVERIFY(!core.player()->radioActive());
        QVERIFY(core.activeTasteSeedName().isEmpty());
        QCOMPARE(core.player()->queue().size(), queue.size());
        QVERIFY(core.settings()->setting(QStringLiteral("radio.session.state")).isEmpty());
        QCOMPARE(core.handleIpcCommand(QStringLiteral("start-taste-radio"),
                                       {{QStringLiteral("name"), QStringLiteral("Absent seed")}})
                     .value(QStringLiteral("radio")).toString(),
                 QStringLiteral("unavailable-taste-seed"));
    }

    void malformedState_data()
    {
        QTest::addColumn<QJsonValue>("seed");
        const QJsonObject valid{{QStringLiteral("format"), QStringLiteral("muzaiten-taste-profile")},
            {QStringLiteral("version"), 1}, {QStringLiteral("genres"), QJsonObject{{QStringLiteral("jazz"), 1.0}}},
            {QStringLiteral("features"), QJsonObject{}}};
        QTest::newRow("missing") << QJsonValue(QJsonObject{});
        QTest::newRow("wrong-type") << QJsonValue(QStringLiteral("seed"));
        QTest::newRow("bad-name") << QJsonValue(QJsonObject{{QStringLiteral("name"), QStringLiteral("bad/name")}, {QStringLiteral("profile"), valid}});
        for (const auto &[label, key, value] : {
                 std::tuple{"version", QStringLiteral("version"), QJsonValue(2)},
                 std::tuple{"shares", QStringLiteral("genres"), QJsonValue(QJsonObject{{QStringLiteral("jazz"), 0.7}})},
                 std::tuple{"quartiles", QStringLiteral("features"), QJsonValue(QJsonObject{{QStringLiteral("energy"),
                     QJsonObject{{QStringLiteral("q25"), 0.8}, {QStringLiteral("q50"), 0.4}, {QStringLiteral("q75"), 0.6}}}})}}) {
            QJsonObject bad = valid;
            bad.insert(key, value);
            QTest::newRow(label) << QJsonValue(QJsonObject{{QStringLiteral("name"), QStringLiteral("Seed")}, {QStringLiteral("profile"), bad}});
        }
    }

    void malformedState()
    {
        QFETCH(QJsonValue, seed);
        const QJsonObject state{{QStringLiteral("active"), true}, {QStringLiteral("kind"), QStringLiteral("taste")},
            {QStringLiteral("tasteSeed"), seed}};
        {
            SettingsStore saved(QDir(m_root->path()).filePath(QStringLiteral("state/state.sqlite")));
            QVERIFY(saved.isOpen());
            QVERIFY(saved.setSetting(QStringLiteral("radio.session.state"),
                                     QString::fromUtf8(QJsonDocument(state).toJson())));
        }
        AppCore core(new FakePlayback, nullptr);
        QVERIFY(!core.player()->radioActive());
        QVERIFY(core.activeTasteSeedName().isEmpty());
        QVERIFY(core.settings()->setting(QStringLiteral("radio.session.state")).isEmpty());
    }
private:
    std::unique_ptr<QTemporaryDir> m_root;
};

QTEST_MAIN(TasteRadioAppCoreTest)
#include "test_taste_radio_appcore.moc"
