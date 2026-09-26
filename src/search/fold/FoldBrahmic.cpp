// Brahmic readings: the Indic scripts, Sinhala, Myanmar and Khmer.
//
// A consonant carries an inherent vowel (a; o for Khmer's second series)
// that a vowel sign replaces and a virama cancels, joining the consonant to
// the next one. The text keeps its surface in the primary fold; three
// readings are alternates:
//
// - style 1, common: long vowels written once (aa as a), and for Hindi and
//   its neighbours the inherent vowel dropped where speech drops it, at the
//   end of a word and between a vowel-consonant and a consonant-vowel
//   (कमला kamla, दिलवाले dilvale, समझ samajh);
// - style 2, full: every inherent vowel kept (kamala), as Sanskrit and the
//   Dravidian languages read;
// - style 3, the ee/oo spelling of film titles and names (दीवाना deewana,
//   फूल phool), with व as w.
//
// An anusvara reads m before p, b and m and n elsewhere; Bengali reads it ng
// (বাংলা bangla), the southern scripts m at the end of a word (മലയാളം
// malayalam), and Khmer m. A nukta turns a consonant into its borrowed
// sound (ज़ z, फ़ f). Khmer drops the inherent vowel of a word's last
// consonant (ខ្មែរ khmaer).

#include "search/fold/FoldEngine.h"

namespace Search::Fold::Engine {

namespace {

using Spelling = QVarLengthArray<char16_t, 8>;

enum class VowelKind : std::uint8_t { Inherent, Written, None };

struct Syllable {
    Spelling consonant;
    Spelling vowel;
    Spelling coda;
    VowelKind kind = VowelKind::None;
    bool hasConsonant = false;
    bool deleted = false;
    int src = 0;
    int vowelSrc = 0;
    int codaSrc = 0;
    bool nasalCoda = false;  // anusvara: its sound depends on the next consonant

    bool hasCoda() const { return nasalCoda || !coda.isEmpty(); }
};

const BrahmicChar *charOf(char32_t cp, BrahmicReading *reading = nullptr)
{
    for (std::size_t k = 0; k < kBrahmicBlockCount; ++k) {
        const BrahmicBlock &b = kBrahmicBlocks[k];
        if (cp >= b.first && cp <= b.last) {
            if (reading) *reading = b.reading;
            return &kBrahmicChars[b.offset + (cp - b.first)];
        }
    }
    return nullptr;
}

const BrahmicPair *findPair(const BrahmicPair *pairs, std::size_t count, char32_t first, char32_t second)
{
    for (std::size_t k = 0; k < count; ++k) {
        if (pairs[k].first == first && pairs[k].second == second) return &pairs[k];
    }
    return nullptr;
}

void append(Spelling &s, QStringView v)
{
    s.append(v.utf16(), v.size());
}

bool isVowelLetter(char16_t c)
{
    return c == u'a' || c == u'e' || c == u'i' || c == u'o' || c == u'u';
}

// A vowel's spelling in `style`: long vowels written once, or as ee/oo.
void spellVowel(QStringView v, int style, Spelling &out)
{
    for (qsizetype k = 0; k < v.size(); ++k) {
        const char16_t c = v[k].unicode();
        if (k + 1 < v.size() && v[k + 1] == c && isVowelLetter(c)) {
            if (style == 3 && (c == u'i' || c == u'u')) {
                const char16_t long_ = c == u'i' ? u'e' : u'o';
                out.append(long_);
                out.append(long_);
            } else {
                out.append(c);
            }
            ++k;
            continue;
        }
        out.append(c);
    }
}

bool isSemivowel(const Spelling &consonant)
{
    return consonant.size() == 1
        && (consonant[0] == u'y' || consonant[0] == u'r' || consonant[0] == u'l' || consonant[0] == u'v'
            || consonant[0] == u'w');
}

// Drops the inherent vowel where Hindi speech drops it: at the end of a word
// (दर्द dard, धर्म dharm), except after a cluster ending in a semivowel (मित्र
// mitra, सूर्य surya), and between a vowel-consonant and a consonant-vowel.
void deleteSchwas(QVarLengthArray<Syllable, 16> &syl)
{
    const qsizetype n = syl.size();
    if (n < 2) return;
    const auto voiced = [&](qsizetype k) {
        return k >= 0 && k < n && syl[k].kind != VowelKind::None && !syl[k].deleted;
    };
    Syllable &last = syl[n - 1];
    const bool afterCluster = syl[n - 2].kind == VowelKind::None;
    if (last.kind == VowelKind::Inherent && last.hasConsonant && !last.hasCoda()
        && !(afterCluster && isSemivowel(last.consonant))) {
        last.deleted = true;
    }
    for (qsizetype k = n - 2; k >= 1; --k) {
        Syllable &s = syl[k];
        if (s.kind != VowelKind::Inherent || !s.hasConsonant || s.hasCoda()) continue;
        if (voiced(k - 1) && !syl[k - 1].hasCoda() && syl[k + 1].hasConsonant && voiced(k + 1)) {
            s.deleted = true;
        }
    }
}

char16_t firstLetter(const QVarLengthArray<Syllable, 16> &syl, qsizetype k)
{
    return k < syl.size() && !syl[k].consonant.isEmpty() ? syl[k].consonant[0] : char16_t(0);
}

QStringView anusvara(BrahmicReading reading, char16_t next)
{
    if (next == u'p' || next == u'b' || next == u'm' || reading == BrahmicReading::Khmer) return u"m";
    if (reading == BrahmicReading::Bengali) return next == u'k' || next == u'g' ? u"n" : u"ng";
    if (reading == BrahmicReading::South && next == 0) return u"m";
    return u"n";
}

void emitWord(QVarLengthArray<Syllable, 16> &syl, int style, BrahmicReading reading, Out &out)
{
    if (style != 2) {
        if (reading == BrahmicReading::North || reading == BrahmicReading::Bengali) {
            deleteSchwas(syl);
        } else if (reading == BrahmicReading::Khmer && syl.size() > 1) {
            Syllable &last = syl.back();
            if (last.kind == VowelKind::Inherent && !last.hasCoda()) last.deleted = true;
        }
    }
    for (qsizetype k = 0; k < syl.size(); ++k) {
        Syllable &s = syl[k];
        out.put(QStringView(s.consonant.data(), s.consonant.size()), s.src);
        if (s.kind != VowelKind::None && !s.deleted) {
            Spelling spelled;
            spellVowel(QStringView(s.vowel.data(), s.vowel.size()), style, spelled);
            out.put(QStringView(spelled.data(), spelled.size()), s.vowelSrc);
        }
        if (s.nasalCoda) {
            out.put(anusvara(reading, firstLetter(syl, k + 1)), s.codaSrc);
        } else if (!s.coda.isEmpty()) {
            out.put(QStringView(s.coda.data(), s.coda.size()), s.codaSrc);
        }
    }
    syl.clear();
}

} // namespace

void foldBrahmic(const Unit *u, qsizetype n, int style, Out &out)
{
    QVarLengthArray<Syllable, 16> word;
    BrahmicReading reading = BrahmicReading::Plain;
    bool geminateNext = false;
    for (qsizetype k = 0; k < n; ++k) {
        const BrahmicChar *c = k == 0 ? charOf(u[k].cp, &reading) : charOf(u[k].cp);
        if (!c) continue;
        switch (c->role) {
        case BrahmicRole::Consonant:
        case BrahmicRole::Final: {
            Syllable s;
            s.src = u[k].src;
            s.hasConsonant = true;
            QStringView sound = view(style == 3 ? c->alt : c->roman);
            const BrahmicChar *next = k + 1 < n ? charOf(u[k + 1].cp) : nullptr;
            if (next && next->role == BrahmicRole::Nukta) {
                sound = view(c->nukta);
                ++k;
                next = k + 1 < n ? charOf(u[k + 1].cp) : nullptr;
            }
            // A conjunct read as a unit (ज्ञ gy) respells the previous consonant.
            if (!word.isEmpty() && word.back().kind == VowelKind::None && word.back().hasConsonant && k > 0) {
                for (qsizetype back = k - 1; back >= 0; --back) {
                    const BrahmicChar *prev = charOf(u[back].cp);
                    if (prev && prev->role == BrahmicRole::Consonant) {
                        if (const BrahmicPair *cluster = findPair(kBrahmicClusters, kBrahmicClusterCount,
                                                                  u[back].cp, u[k].cp)) {
                            word.back().consonant.clear();
                            append(word.back().consonant, view(cluster->roman));
                            sound = QStringView();
                        }
                        break;
                    }
                    if (prev && prev->role != BrahmicRole::Virama) break;
                }
            }
            if (geminateNext && !sound.isEmpty()) s.consonant.append(sound[0].unicode());
            geminateNext = false;
            append(s.consonant, sound);
            if (c->role == BrahmicRole::Final) {
                s.kind = VowelKind::None;
                word.append(std::move(s));
                break;
            }
            // Myanmar medials follow the consonant they modify.
            while (next && next->role == BrahmicRole::Medial) {
                append(s.consonant, view(style == 2 ? next->alt : next->roman));
                ++k;
                next = k + 1 < n ? charOf(u[k + 1].cp) : nullptr;
            }
            if (next && next->role == BrahmicRole::Sign) {
                ++k;
                QStringView vowel = view(next->roman);
                if (k + 1 < n) {
                    if (const BrahmicPair *merged = findPair(kBrahmicMerges, kBrahmicMergeCount, u[k].cp, u[k + 1].cp)) {
                        vowel = view(merged->roman);
                        ++k;
                    }
                }
                append(s.vowel, vowel);
                s.kind = VowelKind::Written;
                s.vowelSrc = u[k].src;
            } else if (next && next->role == BrahmicRole::Virama) {
                ++k;
                s.kind = VowelKind::None;
            } else {
                s.vowel.append(c->inherent);
                s.kind = VowelKind::Inherent;
                s.vowelSrc = s.src;
            }
            word.append(std::move(s));
            break;
        }
        case BrahmicRole::Vowel: {
            Syllable s;
            s.src = s.vowelSrc = u[k].src;
            append(s.vowel, view(c->roman));
            s.kind = VowelKind::Written;
            word.append(std::move(s));
            break;
        }
        case BrahmicRole::Sign: {  // a sign without a consonant
            Syllable s;
            s.src = s.vowelSrc = u[k].src;
            append(s.vowel, view(c->roman));
            s.kind = VowelKind::Written;
            word.append(std::move(s));
            break;
        }
        case BrahmicRole::Anusvara:
        case BrahmicRole::Candrabindu:
        case BrahmicRole::Visarga:
            if (word.isEmpty()) break;
            word.back().codaSrc = u[k].src;
            if (c->role == BrahmicRole::Visarga) word.back().coda.append(u'h');
            else if (c->role == BrahmicRole::Anusvara) word.back().nasalCoda = true;
            else word.back().coda.append(u'n');
            break;
        case BrahmicRole::Geminate:
            geminateNext = true;
            break;
        default:
            break;
        }
    }
    emitWord(word, style, reading, out);
}

} // namespace Search::Fold::Engine
