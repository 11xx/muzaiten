// Hangul.
//
// A syllable block is an initial, a vowel and an optional final consonant,
// recovered arithmetically from its code point (conjoining jamo compose the
// same way). Three readings:
//
// - style 0, the primary fold: Revised Romanization syllable by syllable,
//   each final at the sound it has alone (악단광칠 akdangwangchil, 한국어
//   hangukeo). The fold of any run of syllables is a substring of the fold
//   of a word containing it, so a Hangul query finds the word.
// - style 1: Revised Romanization as pronounced, applying the sound changes
//   at each syllable boundary inside a word: liaison (한국어 hangugeo, 좋아
//   joa), nasalization (독립 dongnip), lateralization (신라 silla),
//   aspiration (좋고 joko) and palatalization (같이 gachi).
// - style 2: McCune-Reischauer over the same pronunciation, with plain stops
//   voiced between voiced sounds and voiceless elsewhere (보천보 pochonbo).

#include "search/fold/FoldEngine.h"

namespace Search::Fold::Engine {

namespace {

// Initial consonants, in jamo order.
enum Initial : int {
    G, KK, N, D, TT, R, M, B, PP, S, SS, Null, J, JJ, CH, K, T, P, H,
    Lateral,  // ㄹ read as l after l or n (신라 silla)
};

// The sound a final consonant settles on, indexing HangulStyle::coda.
enum Coda : int { CodaK, CodaN, CodaT, CodaL, CodaM, CodaP, CodaNG, CodaNone };

constexpr int kVowelI = 20;

// Final consonants 0-27 (0 = none): the sound each settles on.
constexpr Coda kFinalCoda[28] = {
    CodaNone, CodaK, CodaK, CodaK, CodaN, CodaN, CodaN, CodaT, CodaL, CodaK, CodaM, CodaL, CodaL, CodaL,
    CodaP, CodaL, CodaM, CodaP, CodaP, CodaT, CodaT, CodaNG, CodaT, CodaT, CodaK, CodaT, CodaP, CodaT,
};

// Before a vowel a final carries over to start the next syllable: what stays
// behind (a final index) and the initial it becomes (-1 when nothing moves;
// ㅎ falls silent).
struct Liaison {
    int kept;
    int carried;
};
constexpr Liaison kLiaison[28] = {
    {0, -1}, {0, G}, {0, KK}, {1, S}, {0, N}, {4, J}, {0, N}, {0, D}, {0, R}, {8, G}, {8, M}, {8, B}, {8, S},
    {8, T}, {8, P}, {0, R}, {0, M}, {0, B}, {17, S}, {0, S}, {0, SS}, {21, -1}, {0, J}, {0, CH}, {0, K},
    {0, T}, {0, P}, {0, -1},
};

constexpr int kFinalNH = 6;   // ㄶ
constexpr int kFinalLH = 15;  // ㅀ
constexpr int kFinalH = 27;   // ㅎ
constexpr int kFinalNG = 21;  // ㅇ

struct Syllable {
    int initial;
    int vowel;
    int final;  // 0 = none
    int src;
};

// What a boundary between two syllables sounds like: the first one's coda
// and the second one's onset.
struct Boundary {
    Coda coda;
    int onset;
};

Coda codaOfFinal(int final)
{
    return kFinalCoda[final];
}

int aspirated(int initial)
{
    switch (initial) {
    case G:
    case KK:
        return K;
    case D:
    case S:
    case SS:
        return T;
    case B:
        return P;
    case J:
        return CH;
    default:
        return initial;
    }
}

Coda nasalized(Coda c)
{
    switch (c) {
    case CodaK:
        return CodaNG;
    case CodaT:
        return CodaN;
    case CodaP:
        return CodaM;
    default:
        return c;
    }
}

bool isObstruent(Coda c)
{
    return c == CodaK || c == CodaT || c == CodaP;
}

// The boundary as written syllable by syllable.
Boundary writtenBoundary(int final, int initial)
{
    return {codaOfFinal(final), initial};
}

// The boundary as pronounced.
Boundary spokenBoundary(int final, int initial, int vowel)
{
    if (final == 0) return {CodaNone, initial};
    if (initial == Null) {
        if (final == kFinalNG) return {CodaNG, Null};
        const Liaison l = kLiaison[final];
        int onset = l.carried < 0 ? Null : l.carried;
        if (vowel == kVowelI && onset == D) onset = J;
        if (vowel == kVowelI && onset == T) onset = CH;
        return {l.kept ? codaOfFinal(l.kept) : CodaNone, onset};
    }
    const bool hFinal = final == kFinalNH || final == kFinalLH || final == kFinalH;
    if (hFinal) {
        const Coda remaining = final == kFinalNH ? CodaN : final == kFinalLH ? CodaL : CodaNone;
        if (initial == G || initial == D || initial == J) return {remaining, aspirated(initial)};
        if (final == kFinalH && initial == N) return {CodaN, N};
        return {remaining, initial};
    }
    const Coda c = codaOfFinal(final);
    if (initial == H && isObstruent(c)) {
        const Liaison l = kLiaison[final];
        int onset = aspirated(l.carried < 0 ? H : l.carried);
        if (vowel == kVowelI && onset == T) onset = CH;
        return {l.kept ? codaOfFinal(l.kept) : CodaNone, onset};
    }
    if (c == CodaL && initial == N) return {CodaL, Lateral};  // 설날 seollal
    if (initial == N || initial == M) return {isObstruent(c) ? nasalized(c) : c, initial};
    if (initial == R) {
        if (c == CodaL || c == CodaN) return {CodaL, Lateral};  // 신라 silla
        return {nasalized(c), N};
    }
    return {c, initial};
}

void putInitial(const HangulStyle &st, int onset, bool voiced, int src, Out &out)
{
    if (onset == Lateral) {
        out.put(st.lateral, src);
        return;
    }
    out.put(voiced ? st.voicedInitial[onset] : st.initial[onset], src);
}

bool voicedCoda(Coda c)
{
    return c == CodaNone || c == CodaN || c == CodaL || c == CodaM || c == CodaNG;
}

void foldWord(const Syllable *syl, qsizetype n, int style, Out &out)
{
    const HangulStyle &st = style == 2 ? kHangulMr : kHangulRr;
    int onset = syl[0].initial;
    bool voiced = false;  // a word starts voiceless
    for (qsizetype k = 0; k < n; ++k) {
        const Syllable &s = syl[k];
        putInitial(st, onset, voiced, s.src, out);
        out.put(st.vowel[s.vowel], s.src);
        Boundary b{codaOfFinal(s.final), 0};
        if (k + 1 < n) {
            b = style == 0 ? writtenBoundary(s.final, syl[k + 1].initial)
                           : spokenBoundary(s.final, syl[k + 1].initial, syl[k + 1].vowel);
            onset = b.onset;
            voiced = voicedCoda(b.coda);
        }
        if (b.coda != CodaNone) out.put(st.coda[b.coda], s.src);
    }
}

// Payload of a modern jamo: 1 + kind * 32 + index (kind 0 initial, 1 vowel, 2 final).
int jamoKind(const Unit &u)
{
    const std::uint32_t p = payloadOf(u.entry);
    return p == 0 || p == 127 ? -1 : int((p - 1) / 32);
}

int jamoIndex(const Unit &u)
{
    return int((payloadOf(u.entry) - 1) % 32);
}

void foldLoneJamo(const Unit &u, Out &out)
{
    const HangulStyle &st = kHangulRr;
    switch (jamoKind(u)) {
    case 0:
        out.put(jamoIndex(u) == Null ? QStringView(u"ng") : view(st.initial[jamoIndex(u)]), u.src);
        return;
    case 1:
        out.put(st.vowel[jamoIndex(u)], u.src);
        return;
    case 2:
        out.put(st.coda[codaOfFinal(jamoIndex(u))], u.src);
        return;
    default:
        emitPrimary(u, out);
        return;
    }
}

} // namespace

void foldHangul(const Unit *u, qsizetype n, int style, Out &out)
{
    QVarLengthArray<Syllable, 32> word;
    const auto flush = [&] {
        if (!word.isEmpty()) foldWord(word.data(), word.size(), style, out);
        word.clear();
    };
    for (qsizetype k = 0; k < n; ++k) {
        const char32_t cp = u[k].cp;
        if (cp >= 0xAC00 && cp <= 0xD7A3) {
            const int s = int(cp - 0xAC00);
            word.append({s / (21 * 28), (s % (21 * 28)) / 28, s % 28, u[k].src});
            continue;
        }
        // Conjoining jamo spelling a syllable: initial, vowel, optional final.
        if (jamoKind(u[k]) == 0 && k + 1 < n && jamoKind(u[k + 1]) == 1) {
            int final = 0;
            if (k + 2 < n && jamoKind(u[k + 2]) == 2) final = jamoIndex(u[k + 2]);
            word.append({jamoIndex(u[k]), jamoIndex(u[k + 1]), final, u[k].src});
            k += final ? 2 : 1;
            continue;
        }
        flush();
        foldLoneJamo(u[k], out);
    }
    flush();
}

} // namespace Search::Fold::Engine
