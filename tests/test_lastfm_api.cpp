#include "scrobble/LastFmApi.h"

#include <QCryptographicHash>
#include <QTest>

class LastFmApiTest final : public QObject {
    Q_OBJECT

private slots:
    void signsAuthSessionExample();
    void signatureExcludesFormatAndCallback();
    void signatureSortsIndexedNamesLiterally();
    void formBodyEncodesUtf8AndArrayNames();
    void parsesAuthTokenSuccess();
    void parsesAuthSessionSuccess();
    void parsesScrobbleSuccess();
    void parsesFailedResponse();
    void mapsRetryPolicy();
    void validatesMetadataWithoutFilenameFallback();
};

void LastFmApiTest::signsAuthSessionExample()
{
    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("api_key"), QStringLiteral("xxxxxxxxxx"));
    LastFmApi::addParam(params, QStringLiteral("method"), QStringLiteral("auth.getSession"));
    LastFmApi::addParam(params, QStringLiteral("token"), QStringLiteral("yyyyyy"));

    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayLiteral("api_keyxxxxxxxxxxmethodauth.getSessiontokenyyyyyyilovecher"), QCryptographicHash::Md5).toHex());
    QCOMPARE(LastFmApi::signature(params, QStringLiteral("ilovecher")), expected);
}

void LastFmApiTest::signatureExcludesFormatAndCallback()
{
    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("api_key"), QStringLiteral("key"));
    LastFmApi::addParam(params, QStringLiteral("format"), QStringLiteral("json"));
    LastFmApi::addParam(params, QStringLiteral("callback"), QStringLiteral("cb"));
    LastFmApi::addParam(params, QStringLiteral("method"), QStringLiteral("auth.getToken"));

    LastFmApi::Params withoutExcluded;
    LastFmApi::addParam(withoutExcluded, QStringLiteral("api_key"), QStringLiteral("key"));
    LastFmApi::addParam(withoutExcluded, QStringLiteral("method"), QStringLiteral("auth.getToken"));
    QCOMPARE(LastFmApi::signature(params, QStringLiteral("secret")), LastFmApi::signature(withoutExcluded, QStringLiteral("secret")));
}

void LastFmApiTest::signatureSortsIndexedNamesLiterally()
{
    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("artist[1]"), QStringLiteral("one"));
    LastFmApi::addParam(params, QStringLiteral("artist[10]"), QStringLiteral("ten"));
    LastFmApi::addParam(params, QStringLiteral("artist[0]"), QStringLiteral("zero"));

    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayLiteral("artist[0]zeroartist[10]tenartist[1]onesecret"), QCryptographicHash::Md5).toHex());
    QCOMPARE(LastFmApi::signature(params, QStringLiteral("secret")), expected);
}

void LastFmApiTest::formBodyEncodesUtf8AndArrayNames()
{
    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("artist[0]"), QStringLiteral("Björk"));
    LastFmApi::addParam(params, QStringLiteral("track[0]"), QStringLiteral("A Song"));

    const QByteArray body = LastFmApi::formBody(params);
    QVERIFY(body.contains("artist[0]=Bj%C3%B6rk"));
    QVERIFY(body.contains("track[0]=A%20Song"));
}

void LastFmApiTest::parsesAuthTokenSuccess()
{
    const QByteArray xml = R"(<lfm status="ok"><token>cf45fe5a3e3cebe168480a086d7fe481</token></lfm>)";
    const LastFmApi::Response response = LastFmApi::parseXml(xml);
    QVERIFY(response.parsed);
    QVERIFY(response.ok);
    QCOMPARE(response.token, QStringLiteral("cf45fe5a3e3cebe168480a086d7fe481"));
}

void LastFmApiTest::parsesAuthSessionSuccess()
{
    const QByteArray xml = R"(<lfm status="ok"><session><name>MyLastFMUsername</name><key>d580d57f32848f5dcf574d1ce18d78b2</key><subscriber>0</subscriber></session></lfm>)";
    const LastFmApi::Response response = LastFmApi::parseXml(xml);
    QVERIFY(response.parsed);
    QVERIFY(response.ok);
    QCOMPARE(response.sessionName, QStringLiteral("MyLastFMUsername"));
    QCOMPARE(response.sessionKey, QStringLiteral("d580d57f32848f5dcf574d1ce18d78b2"));
}

void LastFmApiTest::parsesScrobbleSuccess()
{
    const QByteArray xml = R"(<lfm status="ok"><scrobbles accepted="1" ignored="0"><scrobble><track corrected="0">Test Track</track><ignoredMessage code="0"></ignoredMessage></scrobble></scrobbles></lfm>)";
    const LastFmApi::Response response = LastFmApi::parseXml(xml);
    QVERIFY(response.parsed);
    QVERIFY(response.ok);
    QCOMPARE(response.accepted, 1);
    QCOMPARE(response.ignored, 0);
    QCOMPARE(response.ignoredCodes, QVector<int>{0});
}

void LastFmApiTest::parsesFailedResponse()
{
    const QByteArray xml = R"(<lfm status="failed"><error code="9">Invalid session key</error></lfm>)";
    const LastFmApi::Response response = LastFmApi::parseXml(xml);
    QVERIFY(response.parsed);
    QVERIFY(!response.ok);
    QCOMPARE(response.errorCode, 9);
    QCOMPARE(response.errorMessage, QStringLiteral("Invalid session key"));
}

void LastFmApiTest::mapsRetryPolicy()
{
    LastFmApi::Response response;
    response.parsed = true;
    response.errorCode = 11;
    QCOMPARE(LastFmApi::scrobbleFailureAction(response, false), LastFmApi::FailureAction::RetryLater);
    response.errorCode = 16;
    QCOMPARE(LastFmApi::scrobbleFailureAction(response, false), LastFmApi::FailureAction::RetryLater);
    response.errorCode = 9;
    QCOMPARE(LastFmApi::scrobbleFailureAction(response, false), LastFmApi::FailureAction::Reauthenticate);
    response.errorCode = 6;
    QCOMPARE(LastFmApi::scrobbleFailureAction(response, false), LastFmApi::FailureAction::DropSubmitted);
    QCOMPARE(LastFmApi::scrobbleFailureAction(response, true), LastFmApi::FailureAction::RetryLater);
}

void LastFmApiTest::validatesMetadataWithoutFilenameFallback()
{
    Track track;
    track.filename = QStringLiteral("filename-title.flac");
    track.albumArtistName = QStringLiteral("Album Artist");
    QVERIFY(!LastFmApi::hasMinimumMetadata(track));

    track.title = QStringLiteral("Tagged Title");
    QVERIFY(LastFmApi::hasMinimumMetadata(track));

    track.durationMs = 30000;
    QVERIFY(LastFmApi::isKnownTooShortToScrobble(track));
    track.durationMs = 31000;
    QVERIFY(!LastFmApi::isKnownTooShortToScrobble(track));
}

QTEST_MAIN(LastFmApiTest)
#include "test_lastfm_api.moc"
