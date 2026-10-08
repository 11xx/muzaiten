#include "ExportPublicationFixture.h"
#include "fs/FilePublish.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

#include <cerrno>
#include <cstdio>

using namespace ExportPublicationFixture;

class FilePublishTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void publishesCompleteFile();
    void refusesExistingFile();
    void refusesDestinationAppearingBeforePublication();
    void refusesSymlinkAppearingBeforePublication();
    void replacesExplicitlyAfterDestinationAppeared();
    void replacementKeepsExistingPermissions();
    void fallbackPublishesWithoutHardLinks();
    void fallbackRefusesDestinationAppearingBeforePublication();
    void fallbackFailureKeepsForeignReplacement();
    void fallbackFailureLeavesOwnEntry();
    void unexpectedLinkFailureRemovesTemporary();

private:
    QString freshDirectory(const QString &name) const;

    QTemporaryDir m_temp;
};

void FilePublishTest::init()
{
    QVERIFY(m_temp.isValid());
}

QString FilePublishTest::freshDirectory(const QString &name) const
{
    const QString path = m_temp.filePath(name);
    QDir().mkpath(path);
    return path;
}

void FilePublishTest::publishesCompleteFile()
{
    const QString directory = freshDirectory(QStringLiteral("publish"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Published);
    QCOMPARE(contents(path), exportContent);
    QCOMPARE(permissionBits(path), referencePermissionBits(freshDirectory(QStringLiteral("reference"))));
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
}

void FilePublishTest::refusesExistingFile()
{
    const QString directory = freshDirectory(QStringLiteral("existing"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    writeFile(path, foreignContent);
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Exists);
    QCOMPARE(contents(path), foreignContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
}

void FilePublishTest::refusesDestinationAppearingBeforePublication()
{
    const QString directory = freshDirectory(QStringLiteral("late-file"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    SeamReset reset;
    FilePublish::setBeforePublishHookForTesting([path] { writeFile(path, foreignContent); });
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Exists);
    QCOMPARE(contents(path), foreignContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
}

void FilePublishTest::refusesSymlinkAppearingBeforePublication()
{
    const QString directory = freshDirectory(QStringLiteral("late-symlink"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    const QString target = QDir(directory).filePath(QStringLiteral("target.bin"));
    writeFile(target, foreignContent);
    SeamReset reset;
    FilePublish::setBeforePublishHookForTesting([path, target] { makeSymlink(target, path); });
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Exists);
    QVERIFY(QFileInfo(path).isSymLink());
    QCOMPARE(contents(target), foreignContent);
    QCOMPARE(entries(directory), (QStringList{QStringLiteral("export.json"), QStringLiteral("target.bin")}));
}

void FilePublishTest::replacesExplicitlyAfterDestinationAppeared()
{
    const QString directory = freshDirectory(QStringLiteral("late-replace"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    SeamReset reset;
    FilePublish::setBeforePublishHookForTesting([path] { writeFile(path, foreignContent); });
    QVERIFY(FilePublish::publish(path, exportContent, true) == FilePublish::Outcome::Published);
    QCOMPARE(contents(path), exportContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
}

void FilePublishTest::replacementKeepsExistingPermissions()
{
    const QString directory = freshDirectory(QStringLiteral("replace-permissions"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    writeFile(path, foreignContent);
    const QFileDevice::Permissions restricted = QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ReadGroup;
    QVERIFY(QFile::setPermissions(path, restricted));
    const int before = permissionBits(path);
    QVERIFY(FilePublish::publish(path, exportContent, true) == FilePublish::Outcome::Published);
    QCOMPARE(contents(path), exportContent);
    QCOMPARE(permissionBits(path), before);
}

void FilePublishTest::fallbackPublishesWithoutHardLinks()
{
    const QString directory = freshDirectory(QStringLiteral("fallback"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    SeamReset reset;
    FilePublish::setLinkFunctionForTesting(linkWithoutHardLinks);
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Published);
    QCOMPARE(contents(path), exportContent);
    QCOMPARE(permissionBits(path), referencePermissionBits(freshDirectory(QStringLiteral("reference"))));
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
}

void FilePublishTest::fallbackRefusesDestinationAppearingBeforePublication()
{
    const QString directory = freshDirectory(QStringLiteral("fallback-late"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    SeamReset reset;
    FilePublish::setLinkFunctionForTesting(linkWithoutHardLinks);
    FilePublish::setBeforePublishHookForTesting([path] { writeFile(path, foreignContent); });
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Exists);
    QCOMPARE(contents(path), foreignContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
}

// The destination is replaced by a completed foreign file while its final sync
// fails. The failed publication must not unlink whatever occupies the name.
void FilePublishTest::fallbackFailureKeepsForeignReplacement()
{
    const QString directory = freshDirectory(QStringLiteral("fallback-foreign-sync"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    const QString foreign = QDir(directory).filePath(QStringLiteral("foreign.part"));
    writeFile(foreign, foreignContent);
    SeamReset reset;
    FilePublish::setLinkFunctionForTesting(linkWithoutHardLinks);
    FilePublish::setFallbackSyncForTesting([path, foreign](int) {
        std::rename(QFile::encodeName(foreign).constData(), QFile::encodeName(path).constData());
        errno = EIO;
        return -1;
    });
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Failed);
    QCOMPARE(contents(path), foreignContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
}

// The final sync fails with nothing else touching the name. The failed create
// keeps its own entry, and a later publication without replacement refuses it.
void FilePublishTest::fallbackFailureLeavesOwnEntry()
{
    const QString directory = freshDirectory(QStringLiteral("fallback-own-sync"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    SeamReset reset;
    FilePublish::setLinkFunctionForTesting(linkWithoutHardLinks);
    FilePublish::setFallbackSyncForTesting([](int) {
        errno = EIO;
        return -1;
    });
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Failed);
    QCOMPARE(contents(path), exportContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("export.json")});
    FilePublish::setFallbackSyncForTesting(nullptr);
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Exists);
    QCOMPARE(contents(path), exportContent);
}

void FilePublishTest::unexpectedLinkFailureRemovesTemporary()
{
    const QString directory = freshDirectory(QStringLiteral("link-failure"));
    const QString path = QDir(directory).filePath(QStringLiteral("export.json"));
    SeamReset reset;
    FilePublish::setLinkFunctionForTesting(linkFailingUnexpectedly);
    QVERIFY(FilePublish::publish(path, exportContent, false) == FilePublish::Outcome::Failed);
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(entries(directory), QStringList{});
}

QTEST_APPLESS_MAIN(FilePublishTest)
#include "test_file_publish.moc"
