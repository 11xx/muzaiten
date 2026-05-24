#include "mpd/MpdConfig.h"

#include <QTest>

class MpdConfigTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesCommonPaths();
    void ignoresCommentsOutsideQuotes();
    void exposesDefaultCandidates();
};

void MpdConfigTest::parsesCommonPaths()
{
    const QString text = QStringLiteral(R"(
music_directory        "/gak/music"
playlist_directory      "/home/lobo/.local/share/mpd/playlists"
db_file                "/home/lobo/.local/share/mpd/database"
sticker_file           "/home/lobo/.local/share/mpd/sticker.sql"
metadata_to_use        "artist,album,title,musicbrainz_trackid"

audio_output {
    type "alsa"
    name "FiiO K5 Pro"
}
)");

    const MpdConfig config = MpdConfigParser::parseText(text, QStringLiteral("/tmp/mpd.conf"));
    QCOMPARE(config.path, QStringLiteral("/tmp/mpd.conf"));
    QCOMPARE(config.musicDirectory, QStringLiteral("/gak/music"));
    QCOMPARE(config.playlistDirectory, QStringLiteral("/home/lobo/.local/share/mpd/playlists"));
    QCOMPARE(config.dbFile, QStringLiteral("/home/lobo/.local/share/mpd/database"));
    QCOMPARE(config.stickerFile, QStringLiteral("/home/lobo/.local/share/mpd/sticker.sql"));
    QCOMPARE(config.metadataToUse, QStringLiteral("artist,album,title,musicbrainz_trackid"));
}

void MpdConfigTest::ignoresCommentsOutsideQuotes()
{
    const QString text = QStringLiteral(R"(
music_directory "/music/#kept" # trailing comment
# playlist_directory "/ignored"
playlist_directory '/playlists'
)");

    const MpdConfig config = MpdConfigParser::parseText(text);
    QCOMPARE(config.musicDirectory, QStringLiteral("/music/#kept"));
    QCOMPARE(config.playlistDirectory, QStringLiteral("/playlists"));
}

void MpdConfigTest::exposesDefaultCandidates()
{
    const QStringList candidates = MpdConfigParser::defaultConfigCandidates();
    QVERIFY(candidates.contains(QStringLiteral("/etc/mpd.conf")));
    QVERIFY(!candidates.isEmpty());
}

QTEST_MAIN(MpdConfigTest)
#include "test_mpd_config.moc"
