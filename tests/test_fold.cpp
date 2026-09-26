#include <QTest>
#include <QFile>
#include <QString>
#include <QStringList>

#include "search/fold/Fold.h"

using namespace Search;

namespace {

QStringList segments(const QString &src)
{
    QStringList texts;
    for (const Fold::FoldResult &f : Fold::foldAll(src)) texts.append(f.text);
    return texts;
}

} // namespace

class TestFold : public QObject {
    Q_OBJECT

private slots:
    void goldenCorpus_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QStringList>("expected");

        QFile file(QStringLiteral(FOLD_CASES_PATH));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const QString contents = QString::fromUtf8(file.readAll());
        for (QString line : contents.split(QChar('\n'))) {
            if (line.endsWith(QChar('\r'))) line.chop(1);
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QChar('#'))) continue;

            QStringList fields = line.split(QChar('\t'));
            QVERIFY2(fields.size() >= 2, qPrintable(QStringLiteral("corpus row has no tab: %1").arg(line)));
            const QString input = fields.takeFirst();
            const QByteArray rowName = input.toUtf8();
            QTest::newRow(rowName.constData()) << input << fields;
        }
    }

    // One expected field checks the primary fold; more check every fold, in order.
    void goldenCorpus()
    {
        QFETCH(QString, input);
        QFETCH(QStringList, expected);
        QCOMPARE(Fold::foldText(input), expected.first());
        if (expected.size() > 1) QCOMPARE(segments(input), expected);
    }

    void asciiPassthroughAndLowercase()
    {
        QCOMPARE(Fold::foldText(QStringLiteral("Hello World")), QStringLiteral("hello world"));
        // Spaces and punctuation are preserved (not slug-collapsed).
        QCOMPARE(Fold::foldText(QStringLiteral("a-b_c.d")), QStringLiteral("a-b_c.d"));
        // Lowercase ASCII comes back as the same shared string.
        const QString plain = QString::fromUtf8("already folded");
        QVERIFY(Fold::foldText(plain).isSharedWith(plain));
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

    void kanjiReadingsAreAlternates()
    {
        // Ideographs keep their surface in the primary fold, so typing them
        // matches, including one character of a dictionary word.
        QCOMPARE(Fold::foldText(QString::fromUtf8("東京")), QString::fromUtf8("东京"));
        QVERIFY(Fold::foldText(QString::fromUtf8("東京")).contains(Fold::foldText(QString::fromUtf8("東"))));
        // Word readings beat single-kanji ones (東京 not 東+京), in Hepburn,
        // pinyin and wapuro.
        QCOMPARE(segments(QString::fromUtf8("東京")),
                 (QStringList{QString::fromUtf8("东京"), QStringLiteral("tokyo"), QStringLiteral("dongjing"),
                              QStringLiteral("toukyou")}));
        // The headline goal, end to end: 三線(word) + の(kana) + 花(single kanji).
        QVERIFY(segments(QString::fromUtf8("三線の花")).contains(QStringLiteral("sanshinnohana")));
        // A kanji with no dictionary entry keeps its surface in the Japanese readings.
        QVERIFY(segments(QString::fromUtf8("阿")).contains(QString::fromUtf8("阿")));
    }

    void controlAndFormatCharactersAreDropped()
    {
        const QString src = QStringLiteral("a") + QChar(0x200E) + QStringLiteral("b") + QChar(0x2060)
            + QStringLiteral("c") + QChar(0x1F) + QStringLiteral("d\te");
        const Fold::FoldResult folded = Fold::fold(src);
        QCOMPARE(folded.text, QStringLiteral("abcd\te"));
        QCOMPARE(folded.srcIndex, (QVector<int>{0, 2, 4, 6, 7, 8}));
        QVERIFY(!folded.text.contains(Fold::kSegmentSeparator));
    }

    void idempotentOnFoldedText()
    {
        const QString once = Fold::foldText(QString::fromUtf8("Café SHCHükÖ"));
        QCOMPARE(Fold::foldText(once), once);
    }

    void segmentsJoinDistinctFolds()
    {
        QCOMPARE(Fold::foldSegments(QStringLiteral("Plain Text")), QStringLiteral("plain text"));
        QCOMPARE(Fold::foldSegments(QString::fromUtf8("Müller")),
                 QStringLiteral("muller") + Fold::kSegmentSeparator + QStringLiteral("mueller")
                     + Fold::kSegmentSeparator + QStringLiteral("miller"));
        const QVector<Fold::FoldResult> all = Fold::foldAll(QString::fromUtf8("Müller"));
        QCOMPARE(all.size(), 3);
        QCOMPARE(all.at(1).srcIndex, (QVector<int>{0, 1, 1, 2, 3, 4, 5}));
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

        // Astral (surrogate-pair) kanji passes through as its two UTF-16 units,
        // each mapped to its own source index so a match highlights the whole
        // glyph rather than half a pair.
        const QString astral = QString::fromUcs4(U"\U0002000B"); // 𠀋, 2 QChars
        const Fold::FoldResult ext = Fold::fold(QStringLiteral("a") + astral + QStringLiteral("b"));
        QCOMPARE(ext.text.size(), 4);
        QCOMPARE(ext.srcIndex, (QVector<int>{0, 1, 2, 3}));

        // A Hangul syllable's letters all map to the syllable.
        const Fold::FoldResult hangul = Fold::fold(QString::fromUtf8("a악단"));
        QCOMPARE(hangul.text, QStringLiteral("aakdan"));
        QCOMPARE(hangul.srcIndex, (QVector<int>{0, 1, 1, 2, 2, 2}));
    }

    void sourceIndexMappingForCompatibilityForms()
    {
        const Fold::FoldResult result = Fold::fold(QString::fromUtf8("AｶﾞBﾊﾟC"));
        QCOMPARE(result.text, QStringLiteral("agabpac"));
        QCOMPARE(result.srcIndex, (QVector<int>{0, 1, 1, 3, 4, 4, 6}));

        // A compatibility character folds through its expansion in place.
        const Fold::FoldResult square = Fold::fold(QString::fromUtf8("x㌔"));
        QCOMPARE(square.text, QStringLiteral("xkiro"));
        QCOMPARE(square.srcIndex, (QVector<int>{0, 1, 1, 1, 1}));
    }

    void decomposedInputFoldsLikeComposed()
    {
        // File names written by macOS are NFD: kana carry combining voicing
        // marks and Hangul arrives as conjoining jamo.
        const QString deco = QString::fromUtf8("テ") + QChar(0x3099) + QString::fromUtf8("スコ");
        QCOMPARE(Fold::foldText(deco), QStringLiteral("desuko"));
        const QString pa = QString::fromUtf8("ハ") + QChar(0x309A);
        QCOMPARE(Fold::foldText(pa), QStringLiteral("pa"));
        const QString han = QString(QChar(0x1112)) + QChar(0x1161) + QChar(0x11AB)  // 한
            + QChar(0x1100) + QChar(0x116E) + QChar(0x11A8);                        // 국
        QCOMPARE(Fold::foldText(han), QStringLiteral("hanguk"));
        QCOMPARE(Fold::fold(han).srcIndex, (QVector<int>{0, 0, 0, 3, 3, 3}));
    }

    void iterationMarksUseTheirSourcePosition()
    {
        const Fold::FoldResult kanji = Fold::fold(QString::fromUtf8("阿々"));
        QCOMPARE(kanji.text, QString::fromUtf8("阿阿"));
        QCOMPARE(kanji.srcIndex, (QVector<int>{0, 1}));

        const Fold::FoldResult kana = Fold::fold(QString::fromUtf8("こゝ"));
        QCOMPARE(kana.text, QStringLiteral("koko"));
        QCOMPARE(kana.srcIndex, (QVector<int>{0, 0, 1, 1}));

        const Fold::FoldResult voiced = Fold::fold(QString::fromUtf8("つゞ"));
        QCOMPARE(voiced.text, QStringLiteral("tsuzu"));
        QCOMPARE(voiced.srcIndex, (QVector<int>{0, 0, 0, 1, 1}));
    }

    void alternateIndexMapsSpreadReadingsOverTheirWord()
    {
        const QVector<Fold::FoldResult> all = Fold::foldAll(QString::fromUtf8("東京"));
        const auto hepburn = std::find_if(all.cbegin(), all.cend(),
                                          [](const Fold::FoldResult &f) { return f.text == QStringLiteral("tokyo"); });
        QVERIFY(hepburn != all.cend());
        QCOMPARE(hepburn->srcIndex, (QVector<int>{0, 0, 1, 1, 1}));
    }

    void foldTextMatchesFoldText()
    {
        // The hot-path (foldText) and the highlight-path (fold) must agree.
        const QString s = QString::fromUtf8("Café 三線 がっこう Привет 악단광칠");
        QCOMPARE(Fold::fold(s).text, Fold::foldText(s));
        QCOMPARE(Fold::foldAll(s).first().text, Fold::foldText(s));
    }

    void everySegmentIndexMapMatchesItsText()
    {
        for (const QString &s : {QString::fromUtf8("東京の夜"), QString::fromUtf8("한국어 노래"),
                                 QString::fromUtf8("Ευχαριστώ"), QString::fromUtf8("Їхав Івась")}) {
            for (const Fold::FoldResult &f : Fold::foldAll(s)) {
                QCOMPARE(f.srcIndex.size(), f.text.size());
                for (const int i : f.srcIndex) QVERIFY(i >= 0 && i < s.size());
            }
        }
    }

    void randomTextKeepsTheInvariants()
    {
        // Letters, marks and punctuation of every script the fold reads, mixed
        // at random with lone surrogates and controls (fixed seed).
        const QList<QPair<char32_t, char32_t>> ranges = {
            {0x20, 0x7E}, {0xC0, 0x24F}, {0x300, 0x36F}, {0x370, 0x3FF}, {0x400, 0x52F}, {0x530, 0x58F},
            {0x590, 0x5FF}, {0x600, 0x6FF}, {0x700, 0x74F}, {0x780, 0x7BF}, {0x900, 0xDFF}, {0xE00, 0xEFF},
            {0xF00, 0xFFF}, {0x1000, 0x109F}, {0x10A0, 0x10FF}, {0x1100, 0x11FF}, {0x1780, 0x17FF},
            {0x200B, 0x206F}, {0x3000, 0x30FF}, {0x3130, 0x318F}, {0x4E00, 0x4E80}, {0xAC00, 0xAD00},
            {0xD800, 0xDFFF}, {0xF900, 0xF9FF}, {0xFB00, 0xFEFF}, {0xFF00, 0xFFEF}, {0x1D400, 0x1D4FF},
            {0x1F100, 0x1F1FF}, {0x20000, 0x20100},
        };
        quint32 state = 0x2545F491u;
        const auto next = [&state] {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        for (int round = 0; round < 3000; ++round) {
            QString s;
            const int length = 1 + int(next() % 12);
            for (int k = 0; k < length; ++k) {
                const auto &[lo, hi] = ranges[qsizetype(next() % quint32(ranges.size()))];
                const char32_t cp = lo + char32_t(next() % (hi - lo + 1));
                if (cp >= 0x10000) s += QString::fromUcs4(&cp, 1);
                else s += QChar(char16_t(cp));
            }
            const QVector<Fold::FoldResult> all = Fold::foldAll(s);
            QVERIFY(!all.isEmpty());
            QCOMPARE(all.first().text, Fold::foldText(s));
            QCOMPARE(Fold::fold(s).text, Fold::foldText(s));
            QVERIFY(Fold::foldSegments(s).startsWith(Fold::foldText(s)));
            for (const Fold::FoldResult &f : all) {
                QCOMPARE(f.srcIndex.size(), f.text.size());
                QVERIFY(!f.text.contains(Fold::kSegmentSeparator));
                for (const int i : f.srcIndex) QVERIFY2(i >= 0 && i < s.size(), qPrintable(s));
            }
        }
    }

    void cacheSignatureIsStable()
    {
        QCOMPARE(Fold::cacheSignature(), Fold::cacheSignature());
        QVERIFY(Fold::cacheSignature() != Fold::kVersion);
    }
};

QTEST_APPLESS_MAIN(TestFold)
#include "test_fold.moc"
