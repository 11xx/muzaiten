#include <QTest>
#include <QDirIterator>
#include <QFile>
#include <QSet>
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
        QCOMPARE(Fold::foldText(QString::fromUtf8("がっこう")), QStringLiteral("gakkou"));     // sokuon
        QCOMPARE(Fold::foldText(QString::fromUtf8("ラーメン")), QStringLiteral("ramen"));      // long mark dropped
        // Yōon and small-vowel morae read as one in the wapuro alternate; the
        // primary fold reads each kana on its own.
        QCOMPARE(Fold::foldText(QString::fromUtf8("とうきょう")), QStringLiteral("toukiyou"));
        QVERIFY(segments(QString::fromUtf8("とうきょう")).contains(QStringLiteral("toukyou")));
        QVERIFY(segments(QString::fromUtf8("まっちゃ")).contains(QStringLiteral("matcha")));     // sokuon + ち
    }

    void kanjiReadingsAreAlternates()
    {
        // Ideographs keep their surface in the primary fold, so typing them
        // matches, including one character of a dictionary word.
        QCOMPARE(Fold::foldText(QString::fromUtf8("東京")), QString::fromUtf8("东京"));
        QVERIFY(Fold::foldText(QString::fromUtf8("東京")).contains(Fold::foldText(QString::fromUtf8("東"))));
        // Word readings beat single-kanji ones (東京 not 東+京), in wapuro,
        // Hepburn and pinyin.
        QCOMPARE(segments(QString::fromUtf8("東京")),
                 (QStringList{QString::fromUtf8("东京"), QStringLiteral("toukyou"), QStringLiteral("tokyo"),
                              QStringLiteral("dongjing")}));
        // The headline goal, end to end: 三線(word) + の(kana) + 花(single kanji).
        QVERIFY(segments(QString::fromUtf8("三線の花")).contains(QStringLiteral("sanshinnohana")));
        // A kanji with no dictionary entry keeps its surface in the Japanese readings.
        QVERIFY(segments(QString::fromUtf8("阿")).contains(QString::fromUtf8("阿")));
    }

    void traditionalAndSimplifiedFoldTogetherBeyondTheBmp()
    {
        // Traditional forms outside the BMP fold onto their simplified form
        // like BMP ones do (𡑍 onto 𫭼, 𨧀 onto 𬭊), and a BMP traditional form
        // can fold onto an astral simplified one (欓 onto 𣗋).
        for (const auto &[traditional, simplified] :
             {std::pair(U"\U0002144D", U"\U0002BB7C"), std::pair(U"\U000289C0", U"\U0002CB4A"),
              std::pair(U"\u6B13", U"\U000235CB")}) {
            const QString t = QString::fromUcs4(traditional);
            const QString s = QString::fromUcs4(simplified);
            QCOMPARE(Fold::foldText(t), Fold::foldText(s));
            QCOMPARE(Fold::foldText(t), s);
        }
    }

    void hanSimplificationChainsAreTerminalAndIdempotent()
    {
        for (const auto &[spellings, terminal] :
             {std::pair(QStringList{QString::fromUtf8("鉅"), QString::fromUtf8("钜"), QString::fromUtf8("巨")},
                        QString::fromUtf8("巨")),
              std::pair(QStringList{QString::fromUtf8("鯰"), QString::fromUtf8("鲶"), QString::fromUtf8("鲇")},
                        QString::fromUtf8("鲇"))}) {
            for (const QString &spelling : spellings) QCOMPARE(Fold::foldText(spelling), terminal);
        }

        // A variant folds with the character it is a variant of (辧 of 辨, not 辦).
        QCOMPARE(Fold::foldText(QString::fromUtf8("辧別")), Fold::foldText(QString::fromUtf8("辨別")));

        const QString root = QStringLiteral(FOLD_HAN_DATA_PATH);
        QSet<QString> mapped;
        const auto appendCodePoints = [&mapped](const QString &text) {
            for (const uint value : text.toUcs4()) {
                const char32_t codePoint = value;
                mapped.insert(QString::fromUcs4(&codePoint, 1));
            }
        };
        const auto appendSources = [&](const QString &path, bool authored) {
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
            const QString contents = QString::fromUtf8(file.readAll());
            for (const QString &line : contents.split(QChar('\n'))) {
                if (line.isEmpty() || line.startsWith(QChar('#'))) continue;
                const QStringList columns = line.split(QChar('\t'));
                if (authored) {
                    if (columns.size() == 3
                        && ((columns[1] != QStringLiteral("-") && !columns[1].isEmpty()) || !columns[2].isEmpty())) {
                        appendCodePoints(columns[0]);
                    }
                } else if (columns.size() == 2) {
                    appendCodePoints(columns[1]);
                }
            }
        };

        QDirIterator authored(root + QStringLiteral("/tables/han"), {QStringLiteral("*.tsv")});
        while (authored.hasNext()) appendSources(authored.next(), true);
        appendSources(root + QStringLiteral("/unihan/han_pinyin.tsv"), false);
        appendSources(root + QStringLiteral("/unihan/han_simplified.tsv"), false);
        for (const QString &character : mapped) {
            const QString primary = Fold::foldText(character);
            QCOMPARE(Fold::foldText(primary), primary);
        }
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

    void scriptsChooseTheirReadingsIndependently()
    {
        // Each script's readings combine with every reading of the others.
        const QStringList mixed = segments(QString::fromUtf8("한국어Müller"));
        for (const QString &expected : {QStringLiteral("hangukeomuller"), QStringLiteral("hangugeomuller"),
                                        QStringLiteral("hangukeomueller"), QStringLiteral("hangugeomiller"),
                                        QStringLiteral("hangugomueller")}) {
            QVERIFY2(mixed.contains(expected), qPrintable(expected));
        }
        // A script whose readings all agree takes no combination away.
        QVERIFY(segments(QString::fromUtf8("한국어Müllerα")).contains(QStringLiteral("hangugeomillera")));
        // Kana and kanji still read as one Japanese spelling per fold.
        QVERIFY(segments(QString::fromUtf8("東京の夜")).contains(QStringLiteral("tokyonoyoru")));
        QVERIFY(!segments(QString::fromUtf8("東京の夜")).contains(QStringLiteral("tokyonoye")));
    }

    void secondHanReadingPreservesMixedScriptCombinations()
    {
        const QStringList withoutSecondReading = segments(QString::fromUtf8("ηMüller"));
        QCOMPARE(withoutSecondReading,
                 (QStringList{QStringLiteral("emuller"), QStringLiteral("emueller"), QStringLiteral("emiller"),
                              QStringLiteral("imuller"), QStringLiteral("imueller"), QStringLiteral("imiller")}));

        const QStringList mixed = segments(QString::fromUtf8("ηMüller长"));
        for (const QString &established :
             {QStringLiteral("emuller长"), QStringLiteral("emueller长"), QStringLiteral("emiller长"),
              QStringLiteral("imuller长"), QStringLiteral("imueller长"), QStringLiteral("imiller长"),
              QStringLiteral("emullerchang"), QStringLiteral("emuellerchang"), QStringLiteral("emillerchang"),
              QStringLiteral("imullerchang"), QStringLiteral("imuellerchang"), QStringLiteral("imillerchang")}) {
            QVERIFY2(mixed.contains(established), qPrintable(established));
        }
        QVERIFY(mixed.contains(QStringLiteral("imuellerzhang")));
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
        // Every fold, not just the primary one, and the reading styles that
        // depend on which letters a string contains.
        for (const QString &s : {QString::fromUtf8("Їхав Київ"), QString::fromUtf8("Müller"),
                                 QString::fromUtf8("ヷ"), QString::fromUtf8("Ευχαριστώ"),
                                 QString::fromUtf8("한국어"), QString::fromUtf8("বোন")}) {
            const QString nfd = s.normalized(QString::NormalizationForm_D);
            QVERIFY2(nfd != s || s == QString::fromUtf8("ヷ"), qPrintable(s));
            QCOMPARE(segments(nfd), segments(s));
            for (const Fold::FoldResult &f : Fold::foldAll(nfd)) {
                QCOMPARE(f.srcIndex.size(), f.text.size());
                for (const int i : f.srcIndex) QVERIFY(i >= 0 && i < nfd.size());
            }
        }
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

    void canonicalOrderAndExcludedCompositionsFoldAlike()
    {
        // Tibetan vowel signs in either order, after a letter or opening the
        // text, and Greek dialytika tonos as the excluded U+0344 or as a
        // precomposed letter.
        QCOMPARE(segments(QString::fromUcs4(U"\u0F66\u0F72\u0F71")), segments(QString::fromUcs4(U"\u0F66\u0F71\u0F72")));
        QCOMPARE(segments(QString::fromUcs4(U"\u03B1\u03B9\u0344")), segments(QString::fromUcs4(U"\u03B1\u0390")));
        QCOMPARE(segments(QString::fromUcs4(U"\u0F72\u0F71")), segments(QString::fromUcs4(U"\u0F71\u0F72")));
        // Tibetan vowels that decompose into marks (U+0F73 = U+0F71 U+0F72)
        // reorder among the marks around them, including each other.
        const char32_t markLed[] = {0x0F73, 0x0F75, 0x0F81};
        for (const QString &before : {QString(), QString(QChar(0x0F66)), QString::fromUcs4(U"\u0F66\u0F74")}) {
            for (const char32_t a : markLed) {
                for (const char32_t b : markLed) {
                    const QString s = before + QString::fromUcs4(&a, 1) + QString::fromUcs4(&b, 1);
                    QCOMPARE(segments(s), segments(s.normalized(QString::NormalizationForm_C)));
                    QCOMPARE(Fold::foldText(Fold::foldText(s)), Fold::foldText(s));
                }
            }
        }
        // A mark after them still composes with the base (A + U+0308 = Ä).
        const QString umlaut = QString::fromUcs4(U"A\u0F75\u0308");
        QCOMPARE(segments(umlaut), segments(umlaut.normalized(QString::NormalizationForm_C)));
        // Kannada oo decomposed into three parts composes in two steps.
        QCOMPARE(segments(QString::fromUcs4(U"\u0C95\u0CC6\u0CC2\u0CD5")), segments(QString::fromUcs4(U"\u0C95\u0CCB")));
        const Fold::FoldResult reordered = Fold::fold(QString::fromUcs4(U"x\u0F66\u0F72\u0F71"));
        QCOMPARE(reordered.srcIndex.size(), reordered.text.size());
        for (const int i : reordered.srcIndex) QVERIFY(i >= 0 && i < 4);
    }

    void normalizedCharactersKeepTheirOwnSourceIndex()
    {
        // A character NFC replaces on its own (Kelvin sign, a compatibility
        // ideograph) maps to itself, not to the letter before it; a composed
        // accent maps to its base.
        QCOMPARE(Fold::fold(QString::fromUcs4(U"A\u212A")).srcIndex, (QVector<int>{0, 1}));
        QCOMPARE(Fold::fold(QString::fromUcs4(U"A\uF902")).srcIndex, (QVector<int>{0, 1}));
        const Fold::FoldResult cafe = Fold::fold(QString::fromUcs4(U"Cafe\u0301\u212A"));
        QCOMPARE(cafe.text, QStringLiteral("cafek"));
        QCOMPARE(cafe.srcIndex, (QVector<int>{0, 1, 2, 3, 5}));
        // A Hangul vowel composes only with a leading consonant, a final only
        // with a syllable that lacks one; composed jamo map to their syllable.
        QCOMPARE(Fold::fold(QString::fromUcs4(U"A\u1169")).srcIndex, (QVector<int>{0, 1}));
        QCOMPARE(Fold::fold(QString::fromUcs4(U"\uAC00\u1169\u11A8")).srcIndex, (QVector<int>{0, 0, 1, 2}));
        const Fold::FoldResult jamo = Fold::fold(QString::fromUcs4(U"A\u1112\u1161\u11AB"));
        QCOMPARE(jamo.text, QStringLiteral("ahan"));
        QCOMPARE(jamo.srcIndex, (QVector<int>{0, 1, 1, 1}));
    }

    void iterationMarksUseTheirSourcePosition()
    {
        // The primary fold keeps iteration marks as written, like a query that
        // types them; the alternates expand them at the mark's position.
        QCOMPARE(Fold::foldText(QString::fromUtf8("阿々")), QString::fromUtf8("阿々"));
        const auto expanded = [](const QString &src, const QString &text) {
            for (const Fold::FoldResult &f : Fold::foldAll(src)) {
                if (f.text == text) return f.srcIndex;
            }
            return QVector<int>{-1};
        };
        QCOMPARE(expanded(QString::fromUtf8("阿々"), QString::fromUtf8("阿阿")), (QVector<int>{0, 1}));
        QCOMPARE(expanded(QString::fromUtf8("こゝ"), QStringLiteral("koko")), (QVector<int>{0, 0, 1, 1}));
        QCOMPARE(expanded(QString::fromUtf8("つゞ"), QStringLiteral("tsuzu")), (QVector<int>{0, 0, 0, 1, 1}));
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

    void randomNormalizationSequencesFoldLikeTheirNormalForms()
    {
        // Characters NFC composes, reorders or replaces, mixed at random
        // (fixed seed): bases, marks of many classes, two-part vowel halves,
        // Hangul jamo, starters that decompose into marks, singletons and
        // excluded compositions.
        const char32_t pool[] = {
            u'a', u'e', u'o', u'A', 0x03B1, 0x03B9, 0x03C9, 0x304B, 0x30CF, 0x0915, 0x0F66, 0x0F40,
            0x0300, 0x0301, 0x0308, 0x0316, 0x031B, 0x0327, 0x0345, 0x0340, 0x0343, 0x0344, 0x05B4, 0x093C,
            0x094D, 0x3099, 0x309A, 0x0F71, 0x0F72, 0x0F74, 0x0F80, 0x0F84, 0x0F73, 0x0F75, 0x0F81,
            0x09C7, 0x09BE, 0x09D7, 0x0B47, 0x0B3E, 0x0B56, 0x0B57, 0x0B92, 0x0BD7, 0x0BC6, 0x0BBE, 0x0CC6,
            0x0CBF, 0x0CC2, 0x0CD5, 0x0CD6, 0x0CCA, 0x0D46, 0x0D3E, 0x0D57, 0x0DD9, 0x0DCF, 0x0DDF, 0x0DCA,
            0x1025, 0x102E, 0x1100, 0x1112, 0x1161, 0x1175, 0x11A8, 0x11C2, 0xAC00, 0xAC01, 0xD7A3,
            0x212A, 0x212B, 0x2126, 0xF902, 0x0958, 0xFB1D, 0x05D9,
        };
        quint32 state = 0x9E3779B9u;
        const auto next = [&state] {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        for (int round = 0; round < 20000; ++round) {
            QString s;
            const int length = 1 + int(next() % 8);
            for (int k = 0; k < length; ++k) {
                const char32_t cp = pool[next() % std::size(pool)];
                s += QString::fromUcs4(&cp, 1);
            }
            const QString nfc = s.normalized(QString::NormalizationForm_C);
            QCOMPARE(segments(s), segments(nfc));
            QCOMPARE(segments(s.normalized(QString::NormalizationForm_D)), segments(nfc));
            QCOMPARE(Fold::foldText(Fold::foldText(s)), Fold::foldText(s));
            for (const Fold::FoldResult &f : Fold::foldAll(s)) {
                QCOMPARE(f.srcIndex.size(), f.text.size());
                for (const int i : f.srcIndex) QVERIFY2(i >= 0 && i < s.size(), qPrintable(s));
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
            // Canonically equivalent spellings fold alike, the random one
            // included (its marks may be out of canonical order).
            QCOMPARE(segments(s.normalized(QString::NormalizationForm_D)),
                     segments(s.normalized(QString::NormalizationForm_C)));
            QCOMPARE(segments(s), segments(s.normalized(QString::NormalizationForm_C)));
            QCOMPARE(all.first().text, Fold::foldText(s));
            QCOMPARE(Fold::fold(s).text, Fold::foldText(s));
            QVERIFY(Fold::foldSegments(s).startsWith(Fold::foldText(s)));
            for (const Fold::FoldResult &f : all) {
                QCOMPARE(f.srcIndex.size(), f.text.size());
                QVERIFY(!f.text.contains(Fold::kSegmentSeparator));
                for (const int i : f.srcIndex) QVERIFY2(i >= 0 && i < s.size(), qPrintable(s));
            }
            // A substring cut between starters folds to a substring of some
            // fold of the whole: a query typed in the text's script finds it.
            const QString nfc = s.normalized(QString::NormalizationForm_C);
            const QStringList folds = segments(nfc);
            QList<qsizetype> cuts;
            for (qsizetype i = 0; i <= nfc.size(); ++i) {
                if (i == nfc.size() || (!nfc.at(i).isLowSurrogate() && QChar::combiningClass(nfc.at(i).unicode()) == 0)) {
                    cuts.append(i);
                }
            }
            for (qsizetype a = 0; a + 1 < cuts.size(); ++a) {
                const QString query = Fold::foldText(nfc.mid(cuts[a], cuts[a + 1] - cuts[a])).trimmed();
                if (query.isEmpty()) continue;
                const bool found = std::any_of(folds.cbegin(), folds.cend(),
                                               [&](const QString &f) { return f.contains(query); });
                QVERIFY2(found, qPrintable(nfc.mid(cuts[a], cuts[a + 1] - cuts[a]) + QStringLiteral(" in ") + nfc));
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
