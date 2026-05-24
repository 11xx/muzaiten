#include "fs/LinkRoot.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class PathResolverTest final : public QObject {
    Q_OBJECT

private slots:
    void localPathResolvesToItself();
    void higherPriorityLinkRootWins();
    void mpdRelativeUriUsesMusicDirectory();
    void writeResolutionPrefersWritableMirror();
    void writeResolutionFailsWithoutWritableCandidate();
    void disabledLinkRootIsIgnored();
};

namespace {

QString touchFile(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write("x");
    file.close();
    return path;
}

} // namespace

void PathResolverTest::localPathResolvesToItself()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = touchFile(temp.filePath(QStringLiteral("Artist/Album/01.flac")));
    QVERIFY(!path.isEmpty());

    const PathResolution resolution = PathResolver().resolveLocalPath(path, PathUse::Read);
    QCOMPARE(resolution.preferredPath, QDir::cleanPath(path));
    QVERIFY(resolution.exists);
    QVERIFY(resolution.readable);
}

void PathResolverTest::higherPriorityLinkRootWins()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString low = touchFile(temp.filePath(QStringLiteral("low/Artist/Album/01.flac")));
    const QString high = touchFile(temp.filePath(QStringLiteral("high/Artist/Album/01.flac")));
    QVERIFY(!low.isEmpty());
    QVERIFY(!high.isEmpty());

    QVector<LinkRoot> roots;
    roots.push_back({0, QStringLiteral("low"), QStringLiteral("/gak/music"), temp.filePath(QStringLiteral("low")), 10, true, false, true});
    roots.push_back({0, QStringLiteral("high"), QStringLiteral("/gak/music"), temp.filePath(QStringLiteral("high")), 100, true, false, true});

    const PathResolution resolution = PathResolver(roots).resolveLocalPath(QStringLiteral("/gak/music/Artist/Album/01.flac"), PathUse::Read);
    QCOMPARE(resolution.preferredPath, QDir::cleanPath(high));
}

void PathResolverTest::mpdRelativeUriUsesMusicDirectory()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = touchFile(temp.filePath(QStringLiteral("music/Artist/Album/02.flac")));
    QVERIFY(!path.isEmpty());

    const PathResolution resolution = PathResolver().resolveMpdUri(QStringLiteral("Artist/Album/02.flac"), temp.filePath(QStringLiteral("music")), PathUse::Read);
    QCOMPARE(resolution.preferredPath, QDir::cleanPath(path));
}

void PathResolverTest::writeResolutionPrefersWritableMirror()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString readOnlyPath = touchFile(temp.filePath(QStringLiteral("readonly/Artist/Album/03.flac")));
    const QString writablePath = touchFile(temp.filePath(QStringLiteral("writable/Artist/Album/03.flac")));
    QVERIFY(!readOnlyPath.isEmpty());
    QVERIFY(!writablePath.isEmpty());

    QFile(readOnlyPath).setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);

    QVector<LinkRoot> roots;
    roots.push_back({0, QStringLiteral("writable"), QStringLiteral("/gak/music"), temp.filePath(QStringLiteral("writable")), 100, true, true, true});
    roots.push_back({0, QStringLiteral("readonly"), QStringLiteral("/gak/music"), temp.filePath(QStringLiteral("readonly")), 50, true, false, true});

    const PathResolution resolution = PathResolver(roots).resolveMpdUri(QStringLiteral("Artist/Album/03.flac"), QStringLiteral("/gak/music"), PathUse::Write);
    QCOMPARE(resolution.preferredPath, QDir::cleanPath(writablePath));
    QVERIFY(resolution.writable);
}

void PathResolverTest::writeResolutionFailsWithoutWritableCandidate()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = touchFile(temp.filePath(QStringLiteral("readonly/Artist/Album/04.flac")));
    QVERIFY(!path.isEmpty());
    QFile(path).setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);

    QVector<LinkRoot> roots;
    roots.push_back({0, QStringLiteral("readonly"), QStringLiteral("/gak/music"), temp.filePath(QStringLiteral("readonly")), 100, true, false, true});

    const PathResolution resolution = PathResolver(roots).resolveMpdUri(QStringLiteral("Artist/Album/04.flac"), QStringLiteral("/gak/music"), PathUse::Write);
    QVERIFY(resolution.preferredPath.isEmpty());
    QVERIFY(!resolution.failureReason.isEmpty());
}

void PathResolverTest::disabledLinkRootIsIgnored()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString disabledPath = touchFile(temp.filePath(QStringLiteral("disabled/Artist/Album/05.flac")));
    QVERIFY(!disabledPath.isEmpty());

    QVector<LinkRoot> roots;
    roots.push_back({0, QStringLiteral("disabled"), QStringLiteral("/gak/music"), temp.filePath(QStringLiteral("disabled")), 100, true, false, false});

    const PathResolution resolution = PathResolver(roots).resolveMpdUri(QStringLiteral("Artist/Album/05.flac"), QStringLiteral("/gak/music"), PathUse::Read);
    QVERIFY(resolution.preferredPath.isEmpty());
    QVERIFY(!resolution.failureReason.isEmpty());
}

QTEST_MAIN(PathResolverTest)
#include "test_path_resolver.moc"
