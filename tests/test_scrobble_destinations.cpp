#include "scrobble/ListenBrainzUrl.h"
#include "scrobble/ScrobbleDestination.h"

#include <QTest>

using namespace ScrobbleDestinationConfig;

namespace {

// JSON fixtures read far better without escaped quotes on every key, and moc's
// preprocessor does not cope with raw string literals, so single quotes stand in
// for double quotes here and are swapped back before parsing.
QString json(const QString &singleQuoted)
{
    return QString(singleQuoted).replace(QLatin1Char('\''), QLatin1Char('"'));
}

}   // namespace

class ScrobbleDestinationsTest final : public QObject {
    Q_OBJECT

private slots:
    void defaultsHoldTheReservedDestinations();
    void roundTripsCustomDestinations();
    void roundTripsAwkwardNames();
    void roundTripsPathPrefixedUrls();
    void mintedIdsAreNeverReused();
    void restoresMissingReservedDestinations();
    void correctsTamperedReservedFields();
    void dropsUnusableCustomEntries();
    void recoversSequenceFromIdsWhenCounterIsStale();
    void degradesToDefaultsOnGarbage_data();
    void degradesToDefaultsOnGarbage();
    void tokenKeysArePerDestination();
};

void ScrobbleDestinationsTest::defaultsHoldTheReservedDestinations()
{
    const ScrobbleDestinationSet set = defaults();
    QCOMPARE(set.items.size(), 2);
    QCOMPARE(set.items.at(0).id, lastFmId());
    QCOMPARE(set.items.at(0).type, ScrobbleDestination::Type::LastFm);
    QCOMPARE(set.items.at(1).id, listenBrainzId());
    QCOMPARE(set.items.at(1).apiRoot, ListenBrainzUrl::officialApiRoot());

    for (const ScrobbleDestination &item : set.items) {
        QVERIFY(item.isReserved());
        QVERIFY(!item.enabled);
    }
}

void ScrobbleDestinationsTest::roundTripsCustomDestinations()
{
    ScrobbleDestinationSet set = defaults();
    const QString id = set.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), true);

    const ScrobbleDestinationSet reloaded = fromJson(toJson(set));
    QCOMPARE(reloaded.items.size(), 3);

    const ScrobbleDestination *koito = reloaded.find(id);
    QVERIFY(koito != nullptr);
    QCOMPARE(koito->name, QStringLiteral("Koito"));
    QCOMPARE(koito->apiRoot, QStringLiteral("https://koito.example/1"));
    QCOMPARE(koito->type, ScrobbleDestination::Type::ListenBrainzCompatible);
    QVERIFY(koito->enabled);
    QVERIFY(!koito->isReserved());
    QCOMPARE(reloaded.nextCustomSequence, set.nextCustomSequence);
}

void ScrobbleDestinationsTest::roundTripsAwkwardNames()
{
    // Names are free text and land in a JSON document; quoting, backslashes,
    // newlines and non-Latin scripts must survive the trip verbatim.
    const QStringList names = {
        QStringLiteral("He said \"scrobble\""),
        QStringLiteral("back\\slash"),
        QStringLiteral("two\nlines"),
        QStringLiteral("音楽 サーバー"),
        QStringLiteral("emoji \U0001F3B5"),
        QStringLiteral("   padded   "),
    };

    ScrobbleDestinationSet set = defaults();
    QStringList ids;
    for (const QString &name : names) {
        ids << set.addCustom(name, QStringLiteral("https://example.test/1"), false);
    }

    const ScrobbleDestinationSet reloaded = fromJson(toJson(set));
    for (int index = 0; index < names.size(); ++index) {
        const ScrobbleDestination *item = reloaded.find(ids.at(index));
        QVERIFY(item != nullptr);
        QCOMPARE(item->name, names.at(index));
    }
}

void ScrobbleDestinationsTest::roundTripsPathPrefixedUrls()
{
    ScrobbleDestinationSet set = defaults();
    const QString prefixed = ListenBrainzUrl::normalizeBase(QStringLiteral("https://example.test/music")).apiRoot;
    const QString ported = ListenBrainzUrl::normalizeBase(QStringLiteral("http://127.0.0.1:4110/koito")).apiRoot;
    const QString a = set.addCustom(QStringLiteral("Prefixed"), prefixed, true);
    const QString b = set.addCustom(QStringLiteral("Ported"), ported, true);

    const ScrobbleDestinationSet reloaded = fromJson(toJson(set));
    QCOMPARE(reloaded.find(a)->apiRoot, QStringLiteral("https://example.test/music/1"));
    QCOMPARE(reloaded.find(b)->apiRoot, QStringLiteral("http://127.0.0.1:4110/koito/1"));
}

void ScrobbleDestinationsTest::mintedIdsAreNeverReused()
{
    ScrobbleDestinationSet set = defaults();
    const QString first = set.addCustom(QStringLiteral("A"), QStringLiteral("https://a.test/1"), true);
    const QString second = set.addCustom(QStringLiteral("B"), QStringLiteral("https://b.test/1"), true);
    QVERIFY(first != second);

    // Removing a destination must not hand its id to the next one: delivery rows
    // and tokens are keyed by id, so reuse would resurrect a dead backlog.
    set.items.removeIf([&](const ScrobbleDestination &item) { return item.id == second; });
    ScrobbleDestinationSet reloaded = fromJson(toJson(set));
    const QString third = reloaded.addCustom(QStringLiteral("C"), QStringLiteral("https://c.test/1"), true);
    QVERIFY(third != first);
    QVERIFY(third != second);

    // And the guarantee survives removing every custom entry.
    reloaded.items.removeIf([](const ScrobbleDestination &item) { return !item.isReserved(); });
    const QString fourth = fromJson(toJson(reloaded))
                               .addCustom(QStringLiteral("D"), QStringLiteral("https://d.test/1"), true);
    QVERIFY(fourth != first);
    QVERIFY(fourth != second);
    QVERIFY(fourth != third);
}

void ScrobbleDestinationsTest::restoresMissingReservedDestinations()
{
    const QString document = json(QStringLiteral(
        "{'version':1,'nextCustomSequence':5,'destinations':["
        "{'id':'custom-4','type':'listenbrainz','name':'Koito',"
        "'apiRoot':'https://koito.example/1','enabled':true}]}"));

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.items.size(), 3);
    QVERIFY(set.find(lastFmId()) != nullptr);
    QVERIFY(set.find(listenBrainzId()) != nullptr);
    QVERIFY(set.find(QStringLiteral("custom-4")) != nullptr);
    QCOMPARE(set.find(listenBrainzId())->apiRoot, ListenBrainzUrl::officialApiRoot());
}

void ScrobbleDestinationsTest::correctsTamperedReservedFields()
{
    // The reserved destinations' type and URL are ours to define. A document
    // claiming otherwise must not be able to redirect official ListenBrainz
    // traffic or turn Last.fm into a compatible endpoint.
    const QString document = json(QStringLiteral(
        "{'version':1,'destinations':["
        "{'id':'listenbrainz','type':'listenbrainz','name':'LB',"
        "'apiRoot':'https://evil.test/1','enabled':true},"
        "{'id':'lastfm','type':'listenbrainz','name':'Last.fm',"
        "'apiRoot':'https://evil.test/1','enabled':true}]}"));

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.find(listenBrainzId())->apiRoot, ListenBrainzUrl::officialApiRoot());
    QCOMPARE(set.find(lastFmId())->type, ScrobbleDestination::Type::LastFm);
    QVERIFY(set.find(lastFmId())->apiRoot.isEmpty());
    // Enabled state, unlike identity, is genuinely the user's.
    QVERIFY(set.find(listenBrainzId())->enabled);
}

void ScrobbleDestinationsTest::dropsUnusableCustomEntries()
{
    const QString document = json(QStringLiteral(
        "{'version':1,'destinations':["
        "{'id':'','type':'listenbrainz','name':'No id','apiRoot':'https://a.test/1'},"
        "{'id':'custom-1','type':'listenbrainz','name':'No url','apiRoot':''},"
        "{'id':'custom-2','type':'listenbrainz','name':'Fine','apiRoot':'https://b.test/1'},"
        "{'id':'custom-2','type':'listenbrainz','name':'Duplicate','apiRoot':'https://c.test/1'}]}"));

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.items.size(), 3);   // two reserved plus the one usable custom
    QVERIFY(set.find(QStringLiteral("custom-1")) == nullptr);
    QCOMPARE(set.find(QStringLiteral("custom-2"))->name, QStringLiteral("Fine"));
}

void ScrobbleDestinationsTest::recoversSequenceFromIdsWhenCounterIsStale()
{
    const QString document = json(QStringLiteral(
        "{'version':1,'nextCustomSequence':1,'destinations':["
        "{'id':'custom-9','type':'listenbrainz','name':'Koito','apiRoot':'https://koito.example/1'}]}"));

    ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.addCustom(QStringLiteral("New"), QStringLiteral("https://n.test/1"), false),
             QStringLiteral("custom-10"));
}

void ScrobbleDestinationsTest::degradesToDefaultsOnGarbage_data()
{
    QTest::addColumn<QString>("document");

    QTest::newRow("empty") << QString();
    QTest::newRow("not json") << QStringLiteral("this is not json");
    QTest::newRow("json array") << QStringLiteral("[1,2,3]");
    QTest::newRow("no destinations key") << json(QStringLiteral("{'version':1}"));
    QTest::newRow("empty destinations") << json(QStringLiteral("{'version':1,'destinations':[]}"));
}

void ScrobbleDestinationsTest::degradesToDefaultsOnGarbage()
{
    QFETCH(QString, document);

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.items.size(), 2);
    QVERIFY(set.find(lastFmId()) != nullptr);
    QVERIFY(set.find(listenBrainzId()) != nullptr);
}

void ScrobbleDestinationsTest::tokenKeysArePerDestination()
{
    // Official ListenBrainz keeps its long-standing key, so upgrading does not
    // silently lose a configured token.
    QCOMPARE(tokenSettingKey(listenBrainzId()), QStringLiteral("listenbrainz.token"));
    QCOMPARE(tokenSettingKey(QStringLiteral("custom-1")), QStringLiteral("scrobble.destination.custom-1.token"));
    QVERIFY(tokenSettingKey(QStringLiteral("custom-1")) != tokenSettingKey(QStringLiteral("custom-2")));
}

QTEST_MAIN(ScrobbleDestinationsTest)
#include "test_scrobble_destinations.moc"
