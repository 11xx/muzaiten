#pragma once

// Embedded default Last.fm application credentials.
//
// These are the last-resort default API key and shared secret, used when the
// user has not provided their own via the settings dialog or the
// LASTFM_API_KEY / LASTFM_SHARED_SECRET environment variables. The values are
// injected at build time (see MUZAITEN_LASTFM_* in CMakeLists.txt) so real
// secrets never enter the source tree; an empty default leaves Last.fm
// inactive until a key is configured.

#ifndef MUZAITEN_LASTFM_API_KEY
#define MUZAITEN_LASTFM_API_KEY ""
#endif
#ifndef MUZAITEN_LASTFM_SHARED_SECRET
#define MUZAITEN_LASTFM_SHARED_SECRET ""
#endif

namespace LastFmCredentials {

inline constexpr auto defaultApiKey = MUZAITEN_LASTFM_API_KEY;
inline constexpr auto defaultSharedSecret = MUZAITEN_LASTFM_SHARED_SECRET;

} // namespace LastFmCredentials
