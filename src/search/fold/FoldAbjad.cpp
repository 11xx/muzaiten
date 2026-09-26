// Readings of the abjads: Arabic (with Persian, Urdu, Kurdish and Pashto
// letters), Hebrew (with Yiddish), Syriac and Thaana.
//
// These scripts write consonants and leave short vowels to optional marks.
// Where marks are written they give the vowels. Otherwise the vowel letters
// (Arabic ا و ي, Hebrew א ו י) read as consonants at the start of a word or
// after a vowel and as long vowels between consonants, and the rest of the
// word keeps its consonants. The primary fold keeps the surface; readings
// are alternates:
//
// - Arabic style 1: al-, u and i (محمود mhmud); style 2 also writes the a a
//   searcher types after a word's first consonant (حبيبي habibi, قلبي qalbi);
//   style 3 spells el-, ou, ee and final ah (نور nour, الجديد eljadeed).
// - Hebrew style 1: ב כ פ soft without a dot (v, kh, f), ח ch, ts, final ה a;
//   style 2: hard ב כ פ, h, tz, final ה ah, and the first-consonant a
//   (שלום shalom, תורה torah).
// - Syriac and Thaana read their vowel marks, which Thaana always writes.

#include "search/fold/FoldEngine.h"

namespace Search::Fold::Engine {

namespace {

using Spelling = QVarLengthArray<char16_t, 8>;

const AbjadChar *abjadOf(char32_t cp)
{
    for (std::size_t k = 0; k < kAbjadBlockCount; ++k) {
        const AbjadBlock &b = kAbjadBlocks[k];
        if (cp >= b.first && cp <= b.last) return &kAbjadChars[b.offset + (cp - b.first)];
    }
    return nullptr;
}

bool isMark(AbjadRole r)
{
    return r >= AbjadRole::Vowel;
}

// A letter with the marks written on it.
struct Letter {
    const AbjadChar *c = nullptr;
    char32_t cp = 0;
    int src = 0;
    Spelling vowel;
    bool shadda = false;
    bool sukun = false;
    bool dagesh = false;
    bool shinDot = false;
    bool sinDot = false;
    bool geresh = false;
    bool sheva = false;

    AbjadRole role() const { return c->role; }
};

using Letters = QVarLengthArray<Letter, 16>;

void append(Spelling &s, QStringView v)
{
    s.append(v.utf16(), v.size());
}

Letters lettersOf(const Unit *u, qsizetype n, bool &pointed)
{
    Letters word;
    pointed = false;
    for (qsizetype k = 0; k < n; ++k) {
        const AbjadChar *c = abjadOf(u[k].cp);
        if (!c || c->role == AbjadRole::None) continue;
        if (!isMark(c->role)) {
            Letter l;
            l.c = c;
            l.cp = u[k].cp;
            l.src = u[k].src;
            word.append(std::move(l));
            continue;
        }
        if (word.isEmpty()) continue;
        Letter &l = word.back();
        switch (c->role) {
        case AbjadRole::Vowel:
            append(l.vowel, view(c->value[0]));
            pointed = true;
            break;
        case AbjadRole::Shadda: l.shadda = true; break;
        case AbjadRole::Sukun: l.sukun = true; break;
        case AbjadRole::Dagesh: l.dagesh = true; break;
        case AbjadRole::ShinDot: l.shinDot = true; break;
        case AbjadRole::SinDot: l.sinDot = true; break;
        case AbjadRole::Geresh: l.geresh = true; break;
        case AbjadRole::Sheva:
            l.sheva = true;
            pointed = true;
            break;
        default:
            break;
        }
    }
    return word;
}

bool endsWithVowel(const Out &out, qsizetype start)
{
    if (out.size() <= start) return false;
    const char16_t c = out.last();
    return c == u'a' || c == u'e' || c == u'i' || c == u'o' || c == u'u';
}

bool isConsonantRole(AbjadRole r)
{
    return r == AbjadRole::Consonant || r == AbjadRole::Ain || r == AbjadRole::Hamza || r == AbjadRole::He
        || r == AbjadRole::Shin;
}

void putDoubled(QStringView s, bool doubled, int src, Out &out)
{
    if (doubled && !s.isEmpty()) out.put(s[0].unicode(), src);
    out.put(s, src);
}

} // namespace

void foldArabic(const Unit *u, qsizetype n, int style, Out &out)
{
    bool pointed = false;
    const Letters w = lettersOf(u, n, pointed);
    const qsizetype count = w.size();
    const qsizetype start = out.size();
    const int s = style - 1;
    qsizetype first = 0;
    if (count > 2 && w[0].cp == U'ا' && w[1].cp == U'ل') {
        out.put(kArabicArticle[s], w[0].src);
        first = 2;
    }
    for (qsizetype k = first; k < count; ++k) {
        const Letter &l = w[k];
        const Letter *next = k + 1 < count ? &w[k + 1] : nullptr;
        const bool atStart = k == first;
        const QStringView vowel(l.vowel.data(), l.vowel.size());
        switch (l.role()) {
        case AbjadRole::Consonant:
        case AbjadRole::Ain:
            putDoubled(view(l.c->value[s]), l.shadda, l.src, out);
            if (!vowel.isEmpty()) out.put(vowel, l.src);
            else if (style >= 2 && atStart && !l.sukun && next && isConsonantRole(next->role())) out.put(u'a', l.src);
            break;
        case AbjadRole::Alef:
            if (!vowel.isEmpty()) out.put(vowel, l.src);
            else if (atStart || !endsWithVowel(out, start)) out.put(l.c->value[s], l.src);
            break;
        case AbjadRole::Waw:
            if (!vowel.isEmpty() || atStart || endsWithVowel(out, start)
                || (next && next->role() == AbjadRole::Alef)) {
                putDoubled(u"w", l.shadda, l.src, out);
                out.put(vowel, l.src);
            } else {
                out.put(kArabicWaw[s], l.src);
            }
            break;
        case AbjadRole::Yeh:
            if (!vowel.isEmpty() || atStart || endsWithVowel(out, start)
                || (next && next->role() == AbjadRole::Alef)) {
                putDoubled(u"y", l.shadda, l.src, out);
                out.put(vowel, l.src);
            } else {
                out.put(next ? kArabicYeh[s] : kArabicYeh[0], l.src);
            }
            break;
        case AbjadRole::Maksura:
            out.put(u'a', l.src);
            break;
        case AbjadRole::Marbuta:
            if (next) out.put(u't', l.src);
            else out.put(kArabicMarbuta[s], l.src);
            break;
        case AbjadRole::Hamza:
            out.put(vowel, l.src);
            break;
        case AbjadRole::VowelLetter:
            out.put(l.c->value[s], l.src);
            break;
        default:
            break;
        }
    }
}

void foldHebrew(const Unit *u, qsizetype n, int style, Out &out)
{
    bool pointed = false;
    const Letters w = lettersOf(u, n, pointed);
    const qsizetype count = w.size();
    const qsizetype start = out.size();
    const int s = style - 1;
    for (qsizetype k = 0; k < count; ++k) {
        const Letter &l = w[k];
        const Letter *next = k + 1 < count ? &w[k + 1] : nullptr;
        const bool atStart = k == 0;
        const QStringView vowel = l.sheva ? (atStart ? QStringView(u"e") : QStringView())
                                          : QStringView(l.vowel.data(), l.vowel.size());
        // Without points a searcher's first vowel is usually a (שלום shalom).
        const bool guessA = !pointed && style == 2 && atStart && next && isConsonantRole(next->role());
        switch (l.role()) {
        case AbjadRole::Consonant:
        case AbjadRole::Shin: {
            QStringView spelled = view(l.c->value[s]);
            if (l.role() == AbjadRole::Shin && l.sinDot) spelled = u"s";
            if (l.geresh && l.c->geresh.len) spelled = view(l.c->geresh);
            else if (l.c->soft.len && !atStart && !l.dagesh && (pointed || style == 1)) spelled = view(l.c->soft);
            out.put(spelled, l.src);
            if (!vowel.isEmpty()) out.put(vowel, l.src);
            else if (guessA) out.put(u'a', l.src);
            break;
        }
        case AbjadRole::Alef:
        case AbjadRole::Ain:
            if (!vowel.isEmpty()) out.put(vowel, l.src);
            else if (!pointed && (!next || (atStart && style == 2))) out.put(u'a', l.src);
            break;
        case AbjadRole::Waw:
            if (l.dagesh && vowel.isEmpty() && !atStart) out.put(u'u', l.src);
            else if (vowel == u"o" && !atStart) out.put(u'o', l.src);
            else if (!vowel.isEmpty()) {
                out.put(u'v', l.src);
                out.put(vowel, l.src);
            } else if (next && next->role() == AbjadRole::Waw) {
                out.put(u'v', l.src);
                ++k;
            } else if (atStart || endsWithVowel(out, start)) {
                out.put(u'v', l.src);
            } else {
                out.put(u'o', l.src);
            }
            break;
        case AbjadRole::Yeh:
            if (!vowel.isEmpty()) {
                out.put(u'y', l.src);
                out.put(vowel, l.src);
            } else if (next && next->role() == AbjadRole::Yeh) {
                // A double yod is y at the start of a word, the diphthong ai after a consonant.
                out.put(atStart || endsWithVowel(out, start) ? QStringView(u"y")
                                                             : QStringView(style == 1 ? u"ay" : u"ai"),
                        l.src);
                ++k;
            } else if (atStart || endsWithVowel(out, start)) {
                if (!(endsWithVowel(out, start) && out.last() == u'i')) out.put(u'y', l.src);
            } else {
                out.put(u'i', l.src);
            }
            break;
        case AbjadRole::He:
            if (!next && !l.dagesh && vowel.isEmpty()) {
                // A word-final he marks a vowel: after a pointed one it adds
                // nothing (תּוֹרָה tora, torah), otherwise it spells the a itself.
                if (!endsWithVowel(out, start)) out.put(kHebrewFinalHe[s], l.src);
                else if (style == 2) out.put(u'h', l.src);
            } else {
                out.put(u'h', l.src);
                if (!vowel.isEmpty()) out.put(vowel, l.src);
                else if (guessA) out.put(u'a', l.src);
            }
            break;
        default:
            break;
        }
    }
}

void foldSyriac(const Unit *u, qsizetype n, Out &out)
{
    bool pointed = false;
    const Letters w = lettersOf(u, n, pointed);
    const qsizetype start = out.size();
    for (qsizetype k = 0; k < w.size(); ++k) {
        const Letter &l = w[k];
        const QStringView vowel(l.vowel.data(), l.vowel.size());
        const bool atStart = k == 0;
        switch (l.role()) {
        case AbjadRole::Consonant:
            out.put(l.c->value[0], l.src);
            out.put(vowel, l.src);
            break;
        case AbjadRole::Alef:
        case AbjadRole::Ain:
            if (!vowel.isEmpty()) out.put(vowel, l.src);
            else if (atStart || k + 1 == w.size()) out.put(u'a', l.src);
            break;
        case AbjadRole::Waw:
        case AbjadRole::Yeh: {
            const bool consonant = !vowel.isEmpty() || atStart || endsWithVowel(out, start);
            if (consonant) out.put(l.role() == AbjadRole::Waw ? u'w' : u'y', l.src);
            else out.put(l.role() == AbjadRole::Waw ? u'u' : u'i', l.src);
            out.put(vowel, l.src);
            break;
        }
        default:
            break;
        }
    }
}

void foldThaana(const Unit *u, qsizetype n, Out &out)
{
    bool pointed = false;
    const Letters w = lettersOf(u, n, pointed);
    for (const Letter &l : w) {
        if (l.role() == AbjadRole::Consonant) out.put(l.c->value[0], l.src);
        out.put(QStringView(l.vowel.data(), l.vowel.size()), l.src);
    }
}

} // namespace Search::Fold::Engine
