#pragma once

// Embedded default Last.fm application credentials.
//
// These are the last-resort default API key and shared secret, used when the
// user has not provided their own via the settings dialog or the
// LASTFM_API_KEY / LASTFM_SHARED_SECRET environment variables. Authenticated
// writes still require a per-user OAuth session key, but these are treated as
// secrets regardless and must not be committed with real values. An empty
// default leaves Last.fm inactive until a key is configured.

namespace LastFmCredentials {

inline constexpr auto defaultApiKey = "";
inline constexpr auto defaultSharedSecret = "";

} // namespace LastFmCredentials
