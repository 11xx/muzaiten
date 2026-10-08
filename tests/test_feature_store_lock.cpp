#include "features/FeatureStoreLock.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <system_error>

namespace {

// Canonical spelling of the temporary root, so expected paths survive a symlinked TMPDIR.
QString canonicalRoot(const QTemporaryDir &dir)
{
    return QFileInfo(dir.path()).canonicalFilePath();
}

bool makeSymlink(const QString &target, const QString &path)
{
    std::error_code error;
    std::filesystem::create_symlink(target.toStdString(), path.toStdString(), error);
    return !error;
}

bool touch(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

} // namespace

class FeatureStoreLockTest final : public QObject {
    Q_OBJECT

private slots:
    void directoryAndFileAliasesShareOneIdentity();
    void absentStoreThroughAliasedDirectoryKeepsCanonicalSpelling();
    void danglingSymlinkChainResolvesToTarget();
    void relativeSpellingMatchesAbsolute();
    void prepareDirectoryCreatesAbsentParentsThroughAliases();
    void danglingParentDirectoryCreatesStoreThroughItsTarget();
    void chainedAndRelativeDanglingDirectoriesResolve();
    void dotDotInSymlinkTargetFollowsKernelAfterAliasExpansion();
    void unresolvableSymlinkCycleYieldsNoCoordinate();
    void liveOwnerExcludesEverySpellingOfTheStore();
};

void FeatureStoreLockTest::directoryAndFileAliasesShareOneIdentity()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real")));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/other")));
    QVERIFY(touch(base + QStringLiteral("/real/features.sqlite")));
    QVERIFY(makeSymlink(base + QStringLiteral("/real"), base + QStringLiteral("/alias")));
    QVERIFY(makeSymlink(QStringLiteral("../real/features.sqlite"), base + QStringLiteral("/other/features.sqlite")));

    const QString canonical = base + QStringLiteral("/real/features.sqlite");
    QCOMPARE(FeatureStoreLock::identity(canonical), canonical);
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/alias/features.sqlite")), canonical);
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/other/features.sqlite")), canonical);
    QCOMPARE(FeatureStoreLock::lockPath(base + QStringLiteral("/other/features.sqlite")),
             canonical + QStringLiteral(".lock"));
}

void FeatureStoreLockTest::absentStoreThroughAliasedDirectoryKeepsCanonicalSpelling()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real")));
    QVERIFY(makeSymlink(base + QStringLiteral("/real"), base + QStringLiteral("/alias")));

    // The subdirectory and the store are both absent; identity must not create either.
    const QString expected = base + QStringLiteral("/real/fresh/features.sqlite");
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/alias/fresh/features.sqlite")), expected);
    QVERIFY(!QFileInfo::exists(base + QStringLiteral("/real/fresh")));
}

void FeatureStoreLockTest::danglingSymlinkChainResolvesToTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real")));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/other")));
    QVERIFY(makeSymlink(QStringLiteral("../real/absent.sqlite"), base + QStringLiteral("/other/absent-link.sqlite")));
    QVERIFY(makeSymlink(QStringLiteral("absent-link.sqlite"), base + QStringLiteral("/other/second-link.sqlite")));

    const QString expected = base + QStringLiteral("/real/absent.sqlite");
    QVERIFY(!QFileInfo::exists(expected));
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/other/absent-link.sqlite")), expected);
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/other/second-link.sqlite")), expected);
}

void FeatureStoreLockTest::relativeSpellingMatchesAbsolute()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real")));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/other")));
    QVERIFY(touch(base + QStringLiteral("/real/features.sqlite")));

    const QString previous = QDir::currentPath();
    QVERIFY(QDir::setCurrent(base + QStringLiteral("/real")));
    const QString fromReal = FeatureStoreLock::identity(QStringLiteral("features.sqlite"));
    QVERIFY(QDir::setCurrent(base + QStringLiteral("/other")));
    const QString fromOther = FeatureStoreLock::identity(QStringLiteral("../real/features.sqlite"));
    QVERIFY(QDir::setCurrent(previous));

    const QString canonical = base + QStringLiteral("/real/features.sqlite");
    QCOMPARE(fromReal, canonical);
    QCOMPARE(fromOther, canonical);
}

void FeatureStoreLockTest::prepareDirectoryCreatesAbsentParentsThroughAliases()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real")));
    QVERIFY(makeSymlink(base + QStringLiteral("/real"), base + QStringLiteral("/alias")));

    const QString store = base + QStringLiteral("/alias/fresh/features.sqlite");
    QVERIFY(FeatureStoreLock::prepareDirectory(store));
    QVERIFY(QFileInfo(base + QStringLiteral("/real/fresh")).isDir());
    QVERIFY(FeatureStoreLock::prepareDirectory(store));

    // The lock file can now be created beside the store that a later write creates.
    QLockFile lock(FeatureStoreLock::lockPath(store));
    QVERIFY(lock.tryLock(0));
}

void FeatureStoreLockTest::danglingParentDirectoryCreatesStoreThroughItsTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real")));
    // The directory alias points at a directory that does not exist yet.
    QVERIFY(makeSymlink(base + QStringLiteral("/real/fresh"), base + QStringLiteral("/dangling-alias")));

    const QString typed = base + QStringLiteral("/dangling-alias/features.sqlite");
    const QString expected = base + QStringLiteral("/real/fresh/features.sqlite");
    QCOMPARE(FeatureStoreLock::identity(typed), expected);
    QVERIFY(!QFileInfo::exists(base + QStringLiteral("/real/fresh")));

    QVERIFY(FeatureStoreLock::prepareDirectory(typed));
    QVERIFY(QFileInfo(base + QStringLiteral("/real/fresh")).isDir());

    // The typed spelling creates the store at the identity, and the lock sits beside it.
    QFile store(typed);
    QVERIFY(store.open(QIODevice::WriteOnly));
    store.close();
    QVERIFY(QFileInfo::exists(expected));
    QLockFile lock(FeatureStoreLock::lockPath(typed));
    QVERIFY(lock.tryLock(0));
    QVERIFY(QFileInfo::exists(expected + QStringLiteral(".lock")));
}

void FeatureStoreLockTest::chainedAndRelativeDanglingDirectoriesResolve()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real/other")));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/nest")));
    QVERIFY(makeSymlink(base + QStringLiteral("/real/fresh"), base + QStringLiteral("/first-alias")));
    QVERIFY(makeSymlink(QStringLiteral("first-alias"), base + QStringLiteral("/second-alias")));
    QVERIFY(makeSymlink(QStringLiteral("../real/fresh"), base + QStringLiteral("/nest/relative-alias")));

    const QString expected = base + QStringLiteral("/real/fresh/features.sqlite");
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/second-alias/features.sqlite")), expected);
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/nest/relative-alias/features.sqlite")), expected);
    // `..` after a dangling directory alias continues from its target, as the kernel does once the target exists.
    QCOMPARE(FeatureStoreLock::identity(base + QStringLiteral("/second-alias/../other/features.sqlite")),
             base + QStringLiteral("/real/other/features.sqlite"));
    QVERIFY(!QFileInfo::exists(base + QStringLiteral("/real/fresh")));
}

void FeatureStoreLockTest::dotDotInSymlinkTargetFollowsKernelAfterAliasExpansion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real/deep/x/y")));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real/plain/y")));
    QVERIFY(touch(base + QStringLiteral("/real/deep/x/features.sqlite")));
    QVERIFY(makeSymlink(QStringLiteral("deep/x/y"), base + QStringLiteral("/real/alias")));
    // The kernel expands `alias` before it applies `..`, so the target is real/deep/x/features.sqlite.
    QVERIFY(makeSymlink(QStringLiteral("alias/../features.sqlite"),
                        base + QStringLiteral("/real/through-alias.sqlite")));
    // Controls: `..` that follows only real directories, in a relative and an absolute target.
    QVERIFY(makeSymlink(QStringLiteral("plain/y/../../deep/x/features.sqlite"),
                        base + QStringLiteral("/real/relative-control.sqlite")));
    QVERIFY(makeSymlink(base + QStringLiteral("/real/plain/y/../../deep/x/features.sqlite"),
                        base + QStringLiteral("/absolute-control.sqlite")));

    const QString expected = base + QStringLiteral("/real/deep/x/features.sqlite");
    const QStringList spellings{base + QStringLiteral("/real/through-alias.sqlite"),
                                base + QStringLiteral("/real/relative-control.sqlite"),
                                base + QStringLiteral("/absolute-control.sqlite")};
    for (const QString &spelling : spellings) {
        const QString kernel = QFileInfo(spelling).canonicalFilePath();
        QCOMPARE(kernel, expected);
        QCOMPARE(FeatureStoreLock::identity(spelling), kernel);
    }
    QCOMPARE(FeatureStoreLock::lockPath(base + QStringLiteral("/real/through-alias.sqlite")),
             expected + QStringLiteral(".lock"));
}

void FeatureStoreLockTest::unresolvableSymlinkCycleYieldsNoCoordinate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(makeSymlink(QStringLiteral("b-link"), base + QStringLiteral("/a-link")));
    QVERIFY(makeSymlink(QStringLiteral("a-link"), base + QStringLiteral("/b-link")));
    QVERIFY(makeSymlink(QStringLiteral("self-link"), base + QStringLiteral("/self-link")));
    QVERIFY(makeSymlink(QStringLiteral("loop-dir/sub"), base + QStringLiteral("/loop-dir")));

    const QStringList unresolvable{base + QStringLiteral("/a-link/features.sqlite"),
                                   base + QStringLiteral("/self-link/features.sqlite"),
                                   base + QStringLiteral("/loop-dir/features.sqlite")};
    for (const QString &path : unresolvable) {
        QVERIFY(FeatureStoreLock::identity(path).isEmpty());
        QVERIFY(FeatureStoreLock::lockPath(path).isEmpty());
        QVERIFY(!FeatureStoreLock::prepareDirectory(path));
    }
}

void FeatureStoreLockTest::liveOwnerExcludesEverySpellingOfTheStore()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = canonicalRoot(dir);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/real")));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/other")));
    QVERIFY(touch(base + QStringLiteral("/real/features.sqlite")));
    QVERIFY(makeSymlink(base + QStringLiteral("/real"), base + QStringLiteral("/alias")));
    QVERIFY(makeSymlink(QStringLiteral("../real/features.sqlite"), base + QStringLiteral("/other/features.sqlite")));

    QLockFile owner(FeatureStoreLock::lockPath(base + QStringLiteral("/real/features.sqlite")));
    QVERIFY(owner.tryLock(0));
    const QStringList contenders{base + QStringLiteral("/real/features.sqlite"),
                                 base + QStringLiteral("/alias/features.sqlite"),
                                 base + QStringLiteral("/other/features.sqlite")};
    for (const QString &contender : contenders) {
        QLockFile attempt(FeatureStoreLock::lockPath(contender));
        QVERIFY(!attempt.tryLock(0));
        QVERIFY(attempt.error() == QLockFile::LockFailedError);
    }

    owner.unlock();
    QLockFile released(FeatureStoreLock::lockPath(base + QStringLiteral("/other/features.sqlite")));
    QVERIFY(released.tryLock(0));
}

QTEST_MAIN(FeatureStoreLockTest)
#include "test_feature_store_lock.moc"
