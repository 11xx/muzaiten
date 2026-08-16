#include "scrobble/ListenBrainzHub.h"

#include "scrobble/ListenBrainzScrobbler.h"

#include <QThread>

ListenBrainzHub::ListenBrainzHub(QObject *parent)
    : QObject(parent)
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("listenbrainz"));
    m_thread->start();
    m_probe = createScrobbler();
}

ListenBrainzHub::~ListenBrainzHub()
{
    // The scrobblers live on the worker thread, so they must be torn down by
    // it. Quitting first lets each finish its current reply handler; the
    // deleteLater connections in createScrobbler then run as the loop exits.
    m_thread->quit();
    m_thread->wait(3000);
}

ListenBrainzScrobbler *ListenBrainzHub::createScrobbler()
{
    auto *scrobbler = new ListenBrainzScrobbler;
    scrobbler->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, scrobbler, &QObject::deleteLater);

    connect(scrobbler, &ListenBrainzScrobbler::submissionFailed, this, &ListenBrainzHub::submissionFailed);
    connect(scrobbler, &ListenBrainzScrobbler::backlogProcessed, this, &ListenBrainzHub::backlogProcessed);
    connect(scrobbler, &ListenBrainzScrobbler::disabledAfterFailures, this, &ListenBrainzHub::disabledAfterFailures);
    connect(scrobbler, &ListenBrainzScrobbler::tokenValidated, this, &ListenBrainzHub::tokenValidated);
    return scrobbler;
}

void ListenBrainzHub::configure(const ScrobbleDestinationSet &destinations,
                                const std::function<QString(const QString &)> &tokenFor,
                                bool uploadAllowed, const QString &historyPath)
{
    QSet<QString> live;
    for (const ScrobbleDestination &destination : destinations.items) {
        if (destination.type != ScrobbleDestination::Type::ListenBrainzCompatible) {
            continue;
        }
        live.insert(destination.id);

        ListenBrainzScrobbler *scrobbler = m_scrobblers.value(destination.id);
        if (scrobbler == nullptr) {
            scrobbler = createScrobbler();
            m_scrobblers.insert(destination.id, scrobbler);
        }
        QMetaObject::invokeMethod(scrobbler, "configure", Qt::QueuedConnection,
                                  Q_ARG(QString, destination.id), Q_ARG(QString, destination.name),
                                  Q_ARG(QString, destination.apiRoot), Q_ARG(bool, destination.enabled),
                                  Q_ARG(bool, uploadAllowed), Q_ARG(QString, tokenFor(destination.id)),
                                  Q_ARG(QString, historyPath));
    }

    for (auto it = m_scrobblers.begin(); it != m_scrobblers.end();) {
        if (live.contains(it.key())) {
            ++it;
            continue;
        }
        // Destroyed on its own thread, so a reply handler mid-flight is not
        // pulled out from under it.
        (*it)->deleteLater();
        it = m_scrobblers.erase(it);
    }
}

void ListenBrainzHub::trackStarted(const Track &track)
{
    for (ListenBrainzScrobbler *scrobbler : std::as_const(m_scrobblers)) {
        QMetaObject::invokeMethod(scrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
    }
}

void ListenBrainzHub::resumeTrack(const Track &track, qint64 elapsedMs, bool playing)
{
    for (ListenBrainzScrobbler *scrobbler : std::as_const(m_scrobblers)) {
        QMetaObject::invokeMethod(scrobbler, "resumeTrack", Qt::QueuedConnection, Q_ARG(Track, track),
                                  Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
    }
}

void ListenBrainzHub::playbackStateChanged(bool playing)
{
    for (ListenBrainzScrobbler *scrobbler : std::as_const(m_scrobblers)) {
        QMetaObject::invokeMethod(scrobbler, "playbackStateChanged", Qt::QueuedConnection, Q_ARG(bool, playing));
    }
}

void ListenBrainzHub::resendNowPlaying()
{
    for (ListenBrainzScrobbler *scrobbler : std::as_const(m_scrobblers)) {
        QMetaObject::invokeMethod(scrobbler, "resendNowPlaying", Qt::QueuedConnection);
    }
}

void ListenBrainzHub::uploadBacklog()
{
    for (ListenBrainzScrobbler *scrobbler : std::as_const(m_scrobblers)) {
        QMetaObject::invokeMethod(scrobbler, "uploadBacklog", Qt::QueuedConnection);
    }
}

void ListenBrainzHub::uploadBacklog(const QString &destinationId)
{
    if (ListenBrainzScrobbler *scrobbler = m_scrobblers.value(destinationId)) {
        QMetaObject::invokeMethod(scrobbler, "uploadBacklog", Qt::QueuedConnection);
    }
}

void ListenBrainzHub::validateToken(const QString &destinationId, quint64 requestId, const QString &apiRoot,
                                    const QString &token)
{
    ListenBrainzScrobbler *scrobbler = m_scrobblers.value(destinationId, m_probe);
    QMetaObject::invokeMethod(scrobbler, "validateToken", Qt::QueuedConnection, Q_ARG(QString, destinationId),
                              Q_ARG(quint64, requestId), Q_ARG(QString, apiRoot), Q_ARG(QString, token));
}
