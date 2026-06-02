#pragma once

#include <array>
#include <cstddef>
#include <string>

// Embedded default Last.fm application credentials.
//
// These are the last-resort default API key and shared secret, used when the
// user has not provided their own via the settings dialog or the
// LASTFM_API_KEY / LASTFM_SHARED_SECRET environment variables. The values are
// injected at build time (see MUZAITEN_LASTFM_* in CMakeLists.txt) so real
// secrets never enter the source tree; an empty default leaves Last.fm
// inactive until a key is configured.
//
// The literals are XOR-obfuscated at compile time (consteval) so the plaintext
// never appears in a `strings` dump of the binary and is only reconstructed in
// memory at runtime. IMPORTANT: this is obfuscation, not security. A client
// binary cannot truly hide a shared secret from a determined reverse-engineer,
// since the XOR pad lives in the same binary; this only defeats casual scraping.
// See docs/distribution.md for the full threat model.

#ifndef MUZAITEN_LASTFM_API_KEY
#define MUZAITEN_LASTFM_API_KEY ""
#endif
#ifndef MUZAITEN_LASTFM_SHARED_SECRET
#define MUZAITEN_LASTFM_SHARED_SECRET ""
#endif

namespace LastFmCredentials {
namespace detail {

// Fixed scrambling pad. Not a secret — it only randomizes the stored bytes so
// they are not greppable as plaintext.
inline constexpr unsigned char kPad[] = {
    0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a, 0x7c, 0x15,
    0xf3, 0x9c, 0xc0, 0x60, 0x5c, 0xed, 0xc8, 0x34,
};

template <std::size_t N>
struct Obfuscated {
    std::array<unsigned char, N> bytes{};
    std::size_t length = 0;

    [[nodiscard]] std::string reveal() const {
        std::string out;
        out.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            out.push_back(static_cast<char>(bytes[i] ^ kPad[i % sizeof(kPad)]));
        }
        return out;
    }
};

template <std::size_t N>
consteval Obfuscated<N> obfuscate(const char (&literal)[N]) {
    Obfuscated<N> o;
    o.length = N > 0 ? N - 1 : 0; // drop the trailing NUL
    for (std::size_t i = 0; i < o.length; ++i) {
        o.bytes[i] = static_cast<unsigned char>(
            static_cast<unsigned char>(literal[i]) ^ kPad[i % sizeof(kPad)]);
    }
    return o;
}

inline constexpr auto kApiKey = obfuscate(MUZAITEN_LASTFM_API_KEY);
inline constexpr auto kSharedSecret = obfuscate(MUZAITEN_LASTFM_SHARED_SECRET);

} // namespace detail

[[nodiscard]] inline std::string defaultApiKey() { return detail::kApiKey.reveal(); }
[[nodiscard]] inline std::string defaultSharedSecret() { return detail::kSharedSecret.reveal(); }

} // namespace LastFmCredentials
