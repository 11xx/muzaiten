#include <QTest>
#include <QString>

#include "search/fold/Fold.h"

using namespace Search;

class TestFold : public QObject {
    Q_OBJECT

private slots:
    void asciiPassthroughAndLowercase()
    {
        QCOMPARE(Fold::foldText(QStringLiteral("Hello World")), QStringLiteral("hello world"));
        // Spaces and punctuation are preserved (not slug-collapsed).
        QCOMPARE(Fold::foldText(QStringLiteral("a-b_c.d")), QStringLiteral("a-b_c.d"));
    }

    void latinDiacritics()
    {
        QCOMPARE(Fold::foldText(QString::fromUtf8("Crème brûlée")), QStringLiteral("creme brulee"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("zażółć gęślą")), QStringLiteral("zazolc gesla"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("Straße")), QStringLiteral("strasse"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("naïve")), QStringLiteral("naive"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("Æther œuvre")), QStringLiteral("aether oeuvre"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("Øresund")), QStringLiteral("oresund"));
    }

    void turkish()
    {
        QCOMPARE(Fold::foldText(QString::fromUtf8("İstanbul")), QStringLiteral("istanbul"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("ılık")), QStringLiteral("ilik"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("Şişli")), QStringLiteral("sisli"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("Gümüş Çağ")), QStringLiteral("gumus cag"));
    }

    void greek()
    {
        QCOMPARE(Fold::foldText(QString::fromUtf8("γειά σου")), QStringLiteral("geia soy"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("Θεός")), QStringLiteral("theos"));
    }

    void cyrillic()
    {
        QCOMPARE(Fold::foldText(QString::fromUtf8("Привет мир")), QStringLiteral("privet mir"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("Москва")), QStringLiteral("moskva"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("ёжик")), QStringLiteral("ezhik"));
    }

    void kanaRomaji()
    {
        QCOMPARE(Fold::foldText(QString::fromUtf8("さんしん")), QStringLiteral("sanshin"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("サンシン")), QStringLiteral("sanshin"));   // katakana
        QCOMPARE(Fold::foldText(QString::fromUtf8("さんしんのはな")), QStringLiteral("sanshinnohana"));
        QCOMPARE(Fold::foldText(QString::fromUtf8("とうきょう")), QStringLiteral("toukyou")); // yōon
        QCOMPARE(Fold::foldText(QString::fromUtf8("がっこう")), QStringLiteral("gakkou"));     // sokuon
        QCOMPARE(Fold::foldText(QString::fromUtf8("まっちゃ")), QStringLiteral("matcha"));     // sokuon + ち
        QCOMPARE(Fold::foldText(QString::fromUtf8("ラーメン")), QStringLiteral("ramen"));      // long mark dropped
    }

    void kanjiPassthrough()
    {
        // Word-level kanji readings are a later slice; kanji must pass through
        // unchanged so typing the original character still matches.
        QCOMPARE(Fold::foldText(QString::fromUtf8("三線")), QString::fromUtf8("三線"));
    }

    void idempotentOnFoldedText()
    {
        const QString once = Fold::foldText(QString::fromUtf8("Café SHCHükÖ"));
        QCOMPARE(Fold::foldText(once), once);
    }

    void sourceIndexMapping()
    {
        // 1:1 — accented letter collapses to one base char at the same index.
        const Fold::FoldResult eAcute = Fold::fold(QString::fromUtf8("café"));
        QCOMPARE(eAcute.text, QStringLiteral("cafe"));
        QCOMPARE(eAcute.srcIndex, (QVector<int>{0, 1, 2, 3}));

        // Decomposed input (e + combining acute) deletes the mark; "e" maps to 0.
        const Fold::FoldResult combining = Fold::fold(QStringLiteral("e") + QChar(0x0301));
        QCOMPARE(combining.text, QStringLiteral("e"));
        QCOMPARE(combining.srcIndex, (QVector<int>{0}));

        // Expansion — both output chars of ß map back to its single source index.
        const Fold::FoldResult sharp = Fold::fold(QString::fromUtf8("aßb"));
        QCOMPARE(sharp.text, QStringLiteral("assb"));
        QCOMPARE(sharp.srcIndex, (QVector<int>{0, 1, 1, 2}));

        // Kana expansion — "sa" both map back to the one さ source index.
        const Fold::FoldResult kana = Fold::fold(QString::fromUtf8("さん"));
        QCOMPARE(kana.text, QStringLiteral("san"));
        QCOMPARE(kana.srcIndex, (QVector<int>{0, 0, 1}));
    }

    void foldTextMatchesFoldText()
    {
        // The hot-path (foldText) and the highlight-path (fold) must agree.
        const QString s = QString::fromUtf8("Café 三線 がっこう Привет");
        QCOMPARE(Fold::fold(s).text, Fold::foldText(s));
    }
};

QTEST_APPLESS_MAIN(TestFold)
#include "test_fold.moc"
