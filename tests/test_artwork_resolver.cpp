#include "scanner/ArtworkResolver.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

class ArtworkResolverTest final : public QObject {
    Q_OBJECT

private slots:
    void prefersFolderCoverAndCachesOutsideAlbum();
    void returnsNoneWhenNoCandidateExists();
};

void ArtworkResolverTest::prefersFolderCoverAndCachesOutsideAlbum()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const QString albumDir = temp.filePath(QStringLiteral("album"));
    QVERIFY(QDir().mkpath(albumDir));
    const QString cacheDir = temp.filePath(QStringLiteral("cache"));
    const QString sourcePath = QDir(albumDir).filePath(QStringLiteral("cover.jpg"));

    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(sourcePath));

    ArtworkResolver resolver(cacheDir);
    const ArtworkResult result = resolver.resolveForDirectory(albumDir);
    QCOMPARE(result.source, ArtworkResult::Source::FolderFile);
    QCOMPARE(result.sourcePath, QFileInfo(sourcePath).absoluteFilePath());
    QVERIFY(result.cachePath.startsWith(cacheDir));
    QVERIFY(QFileInfo::exists(result.cachePath));
}

void ArtworkResolverTest::returnsNoneWhenNoCandidateExists()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    ArtworkResolver resolver(temp.filePath(QStringLiteral("cache")));
    const ArtworkResult result = resolver.resolveForDirectory(temp.path());
    QCOMPARE(result.source, ArtworkResult::Source::None);
    QVERIFY(result.cachePath.isEmpty());
}

QTEST_MAIN(ArtworkResolverTest)
#include "test_artwork_resolver.moc"
