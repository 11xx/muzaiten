#pragma once

#include <QString>

// Strict, dependency-free normalization of a user-entered base URL for a
// ListenBrainz-compatible server (listenbrainz.org itself, Koito, or any other
// implementation of the same HTTP API).
//
// The user may type the server root (`https://koito.example`) or the API root
// including its `/1` version segment (`https://koito.example/1`); both denote
// the same server, so both normalize to the same canonical API root. A path
// prefix is preserved, because a compatible server is often mounted under one
// (`https://example/music` serves its API at `https://example/music/1`).
//
// Everything that could silently change where a token is sent is rejected
// rather than repaired: a non-HTTP(S) scheme, embedded credentials, a query
// string, or a fragment. Endpoints are then built by appending a single path
// segment to the canonical root, never by string-substituting a template.
namespace ListenBrainzUrl {

struct Normalized {
    bool valid = false;
    QString error;    // user-facing reason, set only when invalid
    QString apiRoot;  // canonical `scheme://host[:port][/prefix]/1`, no trailing slash
};

Normalized normalizeBase(const QString &input);

QString submitListensUrl(const QString &apiRoot);
QString validateTokenUrl(const QString &apiRoot);

// The API root of the official ListenBrainz service, in canonical form.
QString officialApiRoot();

}   // namespace ListenBrainzUrl
