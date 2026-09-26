// Thai and Lao readings.
//
// Both scripts write some vowels before the consonant they follow in speech
// (เ แ โ ใ ไ), some above or below it, some after it, and leave others
// unwritten; a consonant's sound depends on whether it starts or ends a
// syllable. The parser walks a run syllable by syllable: an optional leading
// vowel, the initial consonant (with a silent leading ห or อ, or a cluster
// second letter ร ล ว), the vowel pattern, and a final consonant. Where no
// vowel is written it reads o between two consonants (คน khon) and a
// elsewhere (สวัสดี sawatdi). A consonant left alone at the end of a word
// starts a syllable of its own with the one before it (กันยายน kanyayon).
//
// Lao letters carry the Thai letter they behave like, so one parser reads
// both, spelling each script with its own values. Style 1 is RTGS (Lao:
// standard), style 2 the common spelling (Lao: French-style).

#include "search/fold/FoldEngine.h"

namespace Search::Fold::Engine {

namespace {

struct Letter {
    const ThaiChar *c;
    int src;
};

using Syllable = QVarLengthArray<char16_t, 16>;

bool isLowSonorant(char16_t t)
{
    return t == u'ง' || t == u'ญ' || t == u'น' || t == u'ม' || t == u'ย' || t == u'ร' || t == u'ล' || t == u'ว';
}

bool isClusterPair(char16_t first, char16_t second)
{
    switch (second) {
    case u'ร':
        return first == u'ก' || first == u'ข' || first == u'ค' || first == u'ต' || first == u'ป' || first == u'พ'
            || first == u'ท' || first == u'บ' || first == u'ด' || first == u'ฟ' || first == u'ส' || first == u'ศ'
            || first == u'ซ';
    case u'ล':
        return first == u'ก' || first == u'ข' || first == u'ค' || first == u'ป' || first == u'พ' || first == u'บ'
            || first == u'ฟ';
    case u'ว':
        return first == u'ก' || first == u'ข' || first == u'ค';
    default:
        return false;
    }
}

class Parser {
public:
    Parser(const Letter *l, qsizetype n, int style, Out &out)
        : m_l(l), m_n(n), m_style(style), m_out(out)
    {
    }

    void run()
    {
        qsizetype i = 0;
        while (i < m_n) {
            const ThaiRole r = role(i);
            if (r == ThaiRole::Repeat) {
                m_out.put(QStringView(m_last.data(), m_last.size()), m_l[i].src);
                ++i;
                continue;
            }
            if (r == ThaiRole::Letter) {
                i = vowelLetter(i);
                commit();
                continue;
            }
            if (r != ThaiRole::Consonant && r != ThaiRole::Pre) {
                ++i;
                continue;
            }
            if (const qsizetype muted = silenced(i)) {
                i += muted;
                continue;
            }
            i = syllable(i);
            commit();
        }
    }

private:
    ThaiRole role(qsizetype j) const { return j < m_n ? m_l[j].c->role : ThaiRole::None; }
    char16_t thai(qsizetype j) const { return j < m_n ? m_l[j].c->thai : char16_t(0); }

    QStringView vowel(ThaiVowel v) const { return view(kThaiVowels[int(v)][m_style]); }
    QStringView initial(qsizetype j) const { return view(m_l[j].c->initial[m_style]); }
    QStringView final(qsizetype j) const { return view(m_l[j].c->final[m_style]); }

    // A vowel sign, post-vowel or tone mark at `j` belongs to the consonant before it.
    bool attachesToConsonant(qsizetype j) const
    {
        const ThaiRole r = role(j);
        return r == ThaiRole::Above || r == ThaiRole::Post || r == ThaiRole::Tone || r == ThaiRole::Cluster;
    }

    // How many consonants without a written vowel run from `j` to the end of
    // the word, or -1 when another written syllable follows them.
    qsizetype bareTail(qsizetype j) const
    {
        qsizetype count = 0;
        for (qsizetype k = j; k < m_n; ++k) {
            if (role(k) == ThaiRole::Repeat || role(k + 1) == ThaiRole::Silencer) return count;
            // Of a consonant pair before a silencer, the first ends a syllable
            // that has no final yet (ปราโมทย์ pramot); after one both are
            // silent (จันทร์ chan).
            if (role(k) == ThaiRole::Consonant && role(k + 1) == ThaiRole::Consonant
                && role(k + 2) == ThaiRole::Silencer) {
                return count == 0 ? 1 : count;
            }
            if (role(k) != ThaiRole::Consonant || attachesToConsonant(k + 1)) return -1;
            ++count;
        }
        return count;
    }

    // The consonant at `j` ends the current syllable. Consonants ending a word
    // with no written vowel read from the right, o between the last two and a
    // before those (นคร nakhon, ยน yon), so only a lone one is a final.
    bool isFinal(qsizetype j) const
    {
        if (role(j) != ThaiRole::Consonant || attachesToConsonant(j + 1)) return false;
        const qsizetype tail = bareTail(j);
        return tail < 0 || tail == 1 || tail >= 4;
    }

    // Letters at `j` a silencer mutes: a consonant, optionally with a vowel
    // sign or a second consonant (ดิ์, ทร์). 0 when none.
    qsizetype silenced(qsizetype j) const
    {
        if (role(j) != ThaiRole::Consonant) return 0;
        if (role(j + 1) == ThaiRole::Silencer) return 2;
        if ((role(j + 1) == ThaiRole::Above || role(j + 1) == ThaiRole::Consonant)
            && role(j + 2) == ThaiRole::Silencer) {
            return 3;
        }
        return 0;
    }

    // ฤ and ฦ: ri before a final consonant (ฤทธิ์ rit), rue otherwise (ฤดู ruedu).
    qsizetype vowelLetter(qsizetype i)
    {
        const Letter &letter = m_l[i++];
        while (role(i) == ThaiRole::Tone) ++i;
        if (letter.c->vowel == ThaiVowel::RUE && isFinal(i) && !silenced(i)) {
            put(vowel(ThaiVowel::RI), letter.src);
            put(final(i), m_l[i].src);
            return i + 1;
        }
        put(vowel(letter.c->vowel), letter.src);
        return i;
    }

    void skipTones(qsizetype &i) const
    {
        while (i < m_n && role(i) == ThaiRole::Tone) ++i;
    }

    void put(QStringView s, int src)
    {
        m_syllable.append(s.utf16(), s.size());
        m_out.put(s, src);
    }

    void commit()
    {
        if (!m_syllable.isEmpty()) m_last = m_syllable;
        m_syllable.clear();
    }

    qsizetype syllable(qsizetype i)
    {
        char16_t pre = 0;
        int preSrc = 0;
        if (role(i) == ThaiRole::Pre) {
            pre = thai(i);
            preSrc = m_l[i].src;
            ++i;
            if (role(i) != ThaiRole::Consonant) {
                put(vowel(pre == u'แ' ? ThaiVowel::AE : pre == u'โ' ? ThaiVowel::O
                          : pre == u'เ' ? ThaiVowel::E : ThaiVowel::AI), preSrc);
                return i;
            }
        }

        // Initial consonant, with a silent leading ห or อ, and a cluster.
        qsizetype onset = i++;
        if (thai(onset) == u'ห' && (role(i) == ThaiRole::Cluster
                                    || (role(i) == ThaiRole::Consonant && isLowSonorant(thai(i))))) {
            onset = i++;                                      // หมา ma, ຫຼວງ luang
        }
        else if (thai(onset) == u'อ' && thai(i) == u'ย' && role(i) == ThaiRole::Consonant) onset = i++;
        skipTones(i);
        QStringView onsetText = initial(onset);
        QStringView clusterText;
        int clusterSrc = 0;
        if (role(i) == ThaiRole::Cluster) {
            clusterText = initial(i);
            clusterSrc = m_l[i++].src;
        } else if (role(i) == ThaiRole::Consonant && isClusterPair(thai(onset), thai(i))
                   && !(thai(i) == u'ร' && thai(i + 1) == u'ร')
                   && (pre != 0 || attachesToConsonant(i + 1) || role(i + 1) == ThaiRole::Consonant)) {
            // ว between two consonants is the vowel ua (ขวด khuat), not a cluster.
            const bool uaVowel = thai(i) == u'ว' && pre == 0 && !attachesToConsonant(i + 1);
            if (!uaVowel) {
                const char16_t first = thai(onset);
                if (thai(i) == u'ร' && first == u'ท') {
                    onsetText = u"s";                         // ทราย sai
                } else if (!(thai(i) == u'ร' && (first == u'ส' || first == u'ศ' || first == u'ซ'))) {
                    clusterText = initial(i);                 // ร after ส is silent (สร้าง sang)
                    clusterSrc = m_l[i].src;
                }
                ++i;
            }
        }
        put(onsetText, m_l[onset].src);
        if (!clusterText.isEmpty()) put(clusterText, clusterSrc);
        skipTones(i);

        // Vowel.
        ThaiVowel v = ThaiVowel::ImplicitA;
        int vowelSrc = pre ? preSrc : m_l[onset].src;
        ThaiVowel above = ThaiVowel::Count;
        if (role(i) == ThaiRole::Above) {
            above = m_l[i].c->vowel;
            vowelSrc = m_l[i].src;
            ++i;
            skipTones(i);
        }
        bool needFinal = false;
        bool open = false;  // the vowel closes the syllable (ใจ, น้ำ, เกาะ, เขา)
        if (pre == u'เ') {
            if (above == ThaiVowel::II && thai(i) == u'ย') { v = ThaiVowel::IA; ++i; }
            else if (above == ThaiVowel::UEE && thai(i) == u'อ') { v = ThaiVowel::UEA; ++i; }
            else if (above == ThaiVowel::O && thai(i) == u'า' && role(i) == ThaiRole::Post) { v = ThaiVowel::AO; ++i; }
            else if (above == ThaiVowel::I) v = ThaiVowel::OE;
            else if (above == ThaiVowel::O) v = ThaiVowel::E;
            else if (above == ThaiVowel::IA) v = ThaiVowel::IA;
            else if (thai(i) == u'า' && role(i) == ThaiRole::Post) {
                ++i;
                if (thai(i) == u'ะ') { v = ThaiVowel::O; ++i; }
                else v = ThaiVowel::AO;
                open = true;
            } else if (thai(i) == u'อ' && role(i) == ThaiRole::Consonant) { v = ThaiVowel::OE; ++i; }
            else if (thai(i) == u'ะ') { v = ThaiVowel::E; ++i; open = true; }
            else if (thai(i) == u'ย' && role(i) == ThaiRole::Consonant && !attachesToConsonant(i + 1)) {
                v = ThaiVowel::OEI;
                ++i;
                open = true;
            } else v = ThaiVowel::E;
            if (v == ThaiVowel::AO) open = true;
            if (thai(i) == u'ะ') { ++i; open = true; }
        } else if (pre == u'แ') {
            v = ThaiVowel::AE;
            if (thai(i) == u'ะ') { ++i; open = true; }
        } else if (pre == u'โ') {
            v = ThaiVowel::O;
            if (thai(i) == u'ะ') { ++i; open = true; }
        } else if (pre == u'ใ' || pre == u'ไ') {
            v = ThaiVowel::AI;
            open = true;
            // ย after ไ is silent (ไทย thai).
            if (thai(i) == u'ย' && role(i) == ThaiRole::Consonant && !attachesToConsonant(i + 1)) ++i;
        } else if (above != ThaiVowel::Count) {
            if ((above == ThaiVowel::A || above == ThaiVowel::O) && thai(i) == u'ว' && role(i) == ThaiRole::Consonant) {
                v = ThaiVowel::UA;
                ++i;
                if (thai(i) == u'ะ') ++i;
            } else {
                v = above;
                if (above == ThaiVowel::UEE && thai(i) == u'อ' && role(i) == ThaiRole::Consonant) ++i;
            }
        } else if (role(i) == ThaiRole::Post) {
            const char16_t p = thai(i++);
            v = p == u'ำ' ? ThaiVowel::AM : ThaiVowel::A;
            open = p != u'า';
            vowelSrc = m_l[i - 1].src;
        } else if (thai(i) == u'ฤ') {
            v = ThaiVowel::RI;
            vowelSrc = m_l[i].src;
            ++i;
        } else if (thai(i) == u'อ' && role(i) == ThaiRole::Consonant && !attachesToConsonant(i + 1)) {
            v = ThaiVowel::OO;
            vowelSrc = m_l[i].src;
            ++i;
        } else if (thai(i) == u'ว' && role(i) == ThaiRole::Consonant && !attachesToConsonant(i + 1)
                   && (role(i + 1) == ThaiRole::Consonant || i + 1 >= m_n)) {
            v = ThaiVowel::UA;
            vowelSrc = m_l[i].src;
            ++i;
        } else if (thai(i) == u'ร' && thai(i + 1) == u'ร') {
            v = ThaiVowel::A;
            vowelSrc = m_l[i].src;
            i += 2;
            if (!isFinal(i)) {
                put(vowel(v), vowelSrc);
                m_out.put(u'n', vowelSrc);
                m_syllable.append(u'n');
                return i;
            }
            needFinal = true;
        } else if (isFinal(i)) {
            v = ThaiVowel::ImplicitO;
            needFinal = true;
        }
        skipTones(i);
        const QStringView spelled = vowel(v);
        put(spelled, vowelSrc);

        // Final consonant, unless a silencer mutes it.
        if (!open && (needFinal || isFinal(i)) && role(i) == ThaiRole::Consonant) {
            const qsizetype f = i++;
            skipTones(i);
            if (role(i) == ThaiRole::Silencer) {
                ++i;
                return i;
            }
            const QStringView value = final(f);
            const bool elided = thai(f) == u'ย' && !spelled.isEmpty() && spelled.back() == u'i';
            if (!elided) put(value, m_l[f].src);
        }
        // Letters a silencer mutes after the syllable (จันทร์ chan, ศาสตร์ sat).
        i += silenced(i);
        while (role(i) == ThaiRole::Silencer || role(i) == ThaiRole::Tone) ++i;
        return i;
    }

    const Letter *m_l;
    qsizetype m_n;
    int m_style;
    Out &m_out;
    Syllable m_syllable;
    Syllable m_last;
};

} // namespace

void foldThai(const Unit *u, qsizetype n, int style, Out &out)
{
    QVarLengthArray<Letter, 64> letters;
    for (qsizetype k = 0; k < n; ++k) {
        const char32_t cp = u[k].cp;
        if (cp >= 0x0E00 && cp <= 0x0E7F) letters.append({&kThaiChars[cp - 0x0E00], u[k].src});
        else if (cp >= 0x0E80 && cp <= 0x0EFF) letters.append({&kLaoChars[cp - 0x0E80], u[k].src});
    }
    Parser(letters.data(), letters.size(), style - 1, out).run();
}

} // namespace Search::Fold::Engine
