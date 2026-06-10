#include "db/PlaylistDatabase.h"

#include <QTemporaryDir>
#include <QtTest>

class TestPlaylistDatabase final : public QObject {
    Q_OBJECT

private slots:
    void createListRenameDelete();
    void itemOrderingAndRemoval();
    void reorderItems();
    void updateItemFields();

private:
    static PlaylistItem makeItem(const QString &path, const QString &title,
                                 const QString &query = {})
    {
        PlaylistItem item;
        item.trackPath = path;
        item.titleSnapshot = title;
        item.artistSnapshot = QStringLiteral("Artist");
        item.query = query;
        return item;
    }
};

void TestPlaylistDatabase::createListRenameDelete()
{
    QTemporaryDir dir;
    PlaylistDatabase db(QStringLiteral("pl-test-1"));
    QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));

    QVERIFY(db.playlists().isEmpty());
    const qint64 id = db.createPlaylist(QStringLiteral("Roadtrip"));
    QVERIFY(id > 0);

    QVector<Playlist> all = db.playlists();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().name, QStringLiteral("Roadtrip"));
    QCOMPARE(all.first().itemCount, 0);
    QVERIFY(all.first().createdAt > 0);

    QVERIFY(db.renamePlaylist(id, QStringLiteral("Roadtrip 2024")));
    QCOMPARE(db.playlist(id).name, QStringLiteral("Roadtrip 2024"));

    QVERIFY(db.deletePlaylist(id));
    QVERIFY(db.playlists().isEmpty());
}

void TestPlaylistDatabase::itemOrderingAndRemoval()
{
    QTemporaryDir dir;
    PlaylistDatabase db(QStringLiteral("pl-test-2"));
    QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
    const qint64 id = db.createPlaylist(QStringLiteral("Mix"));

    const qint64 a = db.addItem(id, makeItem(QStringLiteral("/a.flac"), QStringLiteral("A")));
    const qint64 b = db.addItem(id, makeItem(QStringLiteral("/b.flac"), QStringLiteral("B")));
    db.addItem(id, makeItem(QStringLiteral("/c.flac"), QStringLiteral("C")));
    QVERIFY(a > 0 && b > 0);

    QVector<PlaylistItem> items = db.items(id);
    QCOMPARE(items.size(), 3);
    QCOMPARE(items.at(0).ordinal, 0);
    QCOMPARE(items.at(1).ordinal, 1);
    QCOMPARE(items.at(2).ordinal, 2);
    QCOMPARE(items.at(0).titleSnapshot, QStringLiteral("A"));
    QCOMPARE(db.playlist(id).itemCount, 0); // itemCount only populated by playlists()
    QCOMPARE(db.playlists().first().itemCount, 3);

    // Removing the middle item compacts ordinals to stay contiguous.
    QVERIFY(db.removeItem(b));
    items = db.items(id);
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(0).titleSnapshot, QStringLiteral("A"));
    QCOMPARE(items.at(1).titleSnapshot, QStringLiteral("C"));
    QCOMPARE(items.at(0).ordinal, 0);
    QCOMPARE(items.at(1).ordinal, 1);
}

void TestPlaylistDatabase::reorderItems()
{
    QTemporaryDir dir;
    PlaylistDatabase db(QStringLiteral("pl-test-3"));
    QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
    const qint64 id = db.createPlaylist(QStringLiteral("Mix"));
    const qint64 a = db.addItem(id, makeItem(QStringLiteral("/a.flac"), QStringLiteral("A")));
    const qint64 b = db.addItem(id, makeItem(QStringLiteral("/b.flac"), QStringLiteral("B")));
    const qint64 c = db.addItem(id, makeItem(QStringLiteral("/c.flac"), QStringLiteral("C")));

    QVERIFY(db.reorderItems(id, {c, a, b}));
    const QVector<PlaylistItem> items = db.items(id);
    QCOMPARE(items.at(0).titleSnapshot, QStringLiteral("C"));
    QCOMPARE(items.at(1).titleSnapshot, QStringLiteral("A"));
    QCOMPARE(items.at(2).titleSnapshot, QStringLiteral("B"));
}

void TestPlaylistDatabase::updateItemFields()
{
    QTemporaryDir dir;
    PlaylistDatabase db(QStringLiteral("pl-test-4"));
    QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
    const qint64 id = db.createPlaylist(QStringLiteral("Mix"));
    const qint64 a = db.addItem(id, makeItem(QStringLiteral("/a.flac"), QStringLiteral("A"),
                                             QStringLiteral("orig query")));

    PlaylistItem item = db.items(id).first();
    QCOMPARE(item.status, PlaylistItemStatus::Matched);
    QCOMPARE(item.query, QStringLiteral("orig query"));

    item.id = a;
    item.playlistId = id;
    item.comment = QStringLiteral("from youtube");
    item.query = QStringLiteral("new query");
    item.status = PlaylistItemStatus::Pending;
    QVERIFY(db.updateItem(item));

    const PlaylistItem reloaded = db.items(id).first();
    QCOMPARE(reloaded.comment, QStringLiteral("from youtube"));
    QCOMPARE(reloaded.query, QStringLiteral("new query"));
    QCOMPARE(reloaded.status, PlaylistItemStatus::Pending);
}

QTEST_MAIN(TestPlaylistDatabase)
#include "test_playlist_database.moc"
