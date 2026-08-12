#include "scrobble/ListenBrainzUrl.h"

#include <QStringList>
#include <QUrl>

namespace ListenBrainzUrl {
namespace {

constexpr auto kVersionSegment = "1";

Normalized invalid(const QString &error)
{
    Normalized result;
    result.error = error;
    return result;
}

}   // namespace

Normalized normalizeBase(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        return invalid(QStringLiteral("Enter a server URL."));
    }

    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid()) {
        return invalid(url.errorString().isEmpty() ? QStringLiteral("Not a valid URL.") : url.errorString());
    }

    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        return invalid(QStringLiteral("The URL must start with http:// or https://."));
    }
    if (url.host().isEmpty()) {
        return invalid(QStringLiteral("The URL has no host name."));
    }
    if (!url.userName().isEmpty() || !url.password().isEmpty()) {
        return invalid(QStringLiteral("The URL must not contain a user name or password."));
    }
    if (url.hasQuery()) {
        return invalid(QStringLiteral("The URL must not contain a query string."));
    }
    if (url.hasFragment()) {
        return invalid(QStringLiteral("The URL must not contain a fragment."));
    }

    QStringList segments;
    const QStringList raw = url.path(QUrl::FullyEncoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : raw) {
        if (segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return invalid(QStringLiteral("The URL path must not contain . or .. segments."));
        }
        segments << segment;
    }
    // The user may point either at the server root or at the API root; both name
    // the same server, so an explicit version segment is absorbed rather than
    // doubled.
    if (!segments.isEmpty() && segments.constLast() == QLatin1String(kVersionSegment)) {
        segments.removeLast();
    }
    segments << QString::fromLatin1(kVersionSegment);

    QUrl canonical;
    canonical.setScheme(scheme);
    canonical.setHost(url.host());
    canonical.setPort(url.port());

    Normalized result;
    result.valid = true;
    result.apiRoot = canonical.url(QUrl::FullyEncoded) + QLatin1Char('/') + segments.join(QLatin1Char('/'));
    return result;
}

QString submitListensUrl(const QString &apiRoot)
{
    return apiRoot + QStringLiteral("/submit-listens");
}

QString validateTokenUrl(const QString &apiRoot)
{
    return apiRoot + QStringLiteral("/validate-token");
}

QString officialApiRoot()
{
    return QStringLiteral("https://api.listenbrainz.org/1");
}

}   // namespace ListenBrainzUrl
