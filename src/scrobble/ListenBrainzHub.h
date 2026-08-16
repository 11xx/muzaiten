#pragma once

#include "core/Track.h"
#include "scrobble/ScrobbleDestination.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class ListenBrainzScrobbler;
class QThread;

// Owns every ListenBrainz-compatible scrobbler and the single worker thread
// they share. One thread, not one per destination: these objects spend their
// time waiting on network replies, so a thread each would buy nothing and cost
// a thread per server the user adds.
//
// The hub fans player events out to all live scrobblers and relays their
// signals back with the destination attached. It owns lifetime and routing
// only; retry, backoff, and failure counting stay inside each scrobbler, which
// is what keeps one destination's trouble to itself.
class ListenBrainzHub final : public QObject {
    Q_OBJECT

public:
    explicit ListenBrainzHub(QObject *parent = nullptr);
    ~ListenBrainzHub() override;

    // Brings the live scrobbler set in line with `destinations`. A destination
    // that is still present keeps its existing object, and with it its backoff
    // and in-flight state, so saving unrelated settings does not reset a
    // backing-off server. Destinations that disappeared are destroyed.
    void configure(const ScrobbleDestinationSet &destinations,
                   const std::function<QString(const QString &destinationId)> &tokenFor,
                   bool uploadAllowed, const QString &historyPath);

    void trackStarted(const Track &track);
    void resumeTrack(const Track &track, qint64 elapsedMs, bool playing);
    void playbackStateChanged(bool playing);
    void resendNowPlaying();
    void uploadBacklog();
    void uploadBacklog(const QString &destinationId);

    // Tests a URL and token that need not belong to a configured destination
    // yet, so the manager can validate before saving.
    // `requestId` is echoed back with the result, so a caller that issued more
    // than one test can tell which one an answer belongs to.
    void validateToken(const QString &destinationId, quint64 requestId, const QString &apiRoot, const QString &token);

signals:
    void submissionFailed(QString destinationId, QString message);
    void backlogProcessed(QString destinationId, int sentCount, int skippedCount, int remainingCount);
    void disabledAfterFailures(QString destinationId, QString message);
    void tokenValidated(QString destinationId, quint64 requestId, bool valid, QString username);

private:
    ListenBrainzScrobbler *createScrobbler();

    QThread *m_thread = nullptr;
    QHash<QString, ListenBrainzScrobbler *> m_scrobblers;
    // Serves validation requests for destinations that have no scrobbler yet.
    ListenBrainzScrobbler *m_probe = nullptr;
};
