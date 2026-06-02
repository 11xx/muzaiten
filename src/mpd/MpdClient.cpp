#include "mpd/MpdClient.h"

#include <QTcpSocket>

#include <algorithm>

namespace {

QString quoteArg(QString arg)
{
    arg.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    arg.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(arg);
}

int leadingNumber(const QString &value)
{
    const QStringList parts = value.split(QLatin1Char('/'));
    bool ok = false;
    const int number = parts.value(0).toInt(&ok);
    return ok ? number : 0;
}

void applyField(MpdTrack *track, const QString &key, const QString &value)
{
    const QString lower = key.toLower();
    if (lower == QStringLiteral("file")) {
        track->uri = value;
    } else if (lower == QStringLiteral("title")) {
        track->title = value;
    } else if (lower == QStringLiteral("artist")) {
        track->artistName = value;
    } else if (lower == QStringLiteral("albumartist") || lower == QStringLiteral("album artist")) {
        track->albumArtistName = value;
    } else if (lower == QStringLiteral("album")) {
        track->albumTitle = value;
    } else if (lower == QStringLiteral("track")) {
        track->trackNumber = leadingNumber(value);
    } else if (lower == QStringLiteral("disc")) {
        track->discNumber = leadingNumber(value);
    } else if (lower == QStringLiteral("duration")) {
        track->durationMs = static_cast<qint64>(value.toDouble() * 1000.0);
    } else if (lower == QStringLiteral("time") && track->durationMs <= 0) {
        track->durationMs = static_cast<qint64>(value.toLongLong() * 1000);
    } else if (lower == QStringLiteral("date")) {
        track->date = value;
    } else if (lower == QStringLiteral("musicbrainz_artistid")) {
        track->musicBrainz.artistId = value;
    } else if (lower == QStringLiteral("musicbrainz_albumartistid")) {
        track->musicBrainz.albumArtistId = value;
    } else if (lower == QStringLiteral("musicbrainz_albumid")) {
        track->musicBrainz.releaseId = value;
    } else if (lower == QStringLiteral("musicbrainz_trackid")) {
        track->musicBrainz.recordingId = value;
    } else if (lower == QStringLiteral("musicbrainz_releasetrackid")) {
        track->musicBrainz.trackId = value;
    }
}

QVector<MpdTrack> parseListAllInfo(const QStringList &lines)
{
    QVector<MpdTrack> tracks;
    MpdTrack current;
    bool hasTrack = false;

    for (const QString &line : lines) {
        const qsizetype colon = line.indexOf(QStringLiteral(": "));
        if (colon < 0) {
            continue;
        }

        const QString key = line.left(colon);
        const QString value = line.mid(colon + 2);
        if (key.compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
            if (hasTrack && !current.uri.isEmpty()) {
                tracks.push_back(current);
            }
            current = {};
            hasTrack = true;
        }
        if (hasTrack) {
            applyField(&current, key, value);
        }
    }

    if (hasTrack && !current.uri.isEmpty()) {
        tracks.push_back(current);
    }
    return tracks;
}

} // namespace

MpdClient::MpdClient()
    : m_socket(new QTcpSocket)
{
}

MpdClient::~MpdClient()
{
    delete m_socket;
}

bool MpdClient::isReadOnlyCommand(const QString &command)
{
    static const QStringList allowed = {
        QStringLiteral("listallinfo"),
        QStringLiteral("listplaylists"),
        QStringLiteral("listplaylistinfo"),
        QStringLiteral("lsinfo"),
        QStringLiteral("search"),
        QStringLiteral("find"),
    };
    return allowed.contains(command.toLower());
}

bool MpdClient::connectToServer(const QString &host, quint16 port, int timeoutMs, QString *error)
{
    m_socket->connectToHost(host, port);
    if (!m_socket->waitForConnected(timeoutMs)) {
        if (error != nullptr) {
            *error = m_socket->errorString();
        }
        return false;
    }

    if (!m_socket->waitForReadyRead(timeoutMs)) {
        if (error != nullptr) {
            *error = m_socket->errorString();
        }
        return false;
    }
    const QString greeting = QString::fromUtf8(m_socket->readLine()).trimmed();
    if (!greeting.startsWith(QStringLiteral("OK MPD"))) {
        if (error != nullptr && error->isEmpty()) {
            *error = QStringLiteral("Invalid MPD greeting");
        }
        return false;
    }
    return true;
}

QVector<MpdTrack> MpdClient::listAllInfo(QString *error)
{
    return parseListAllInfo(command(QStringLiteral("listallinfo"), {}, error));
}

QStringList MpdClient::sentCommands() const
{
    return m_sentCommands;
}

QStringList MpdClient::command(const QString &commandName, const QStringList &args, QString *error)
{
    if (!isReadOnlyCommand(commandName)) {
        if (error != nullptr) {
            *error = QStringLiteral("Refusing non-read-only MPD command: %1").arg(commandName);
        }
        return {};
    }

    QString commandLine = commandName;
    for (const QString &arg : args) {
        commandLine += QLatin1Char(' ');
        commandLine += quoteArg(arg);
    }
    commandLine += QLatin1Char('\n');
    m_sentCommands.push_back(commandName);
    m_socket->write(commandLine.toUtf8());
    if (!m_socket->waitForBytesWritten(3000)) {
        if (error != nullptr) {
            *error = m_socket->errorString();
        }
        return {};
    }

    const QString response = readResponse(error);
    return response.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QString MpdClient::readResponse(QString *error)
{
    QByteArray buffer;
    while (true) {
        if (!m_socket->waitForReadyRead(5000)) {
            if (error != nullptr) {
                *error = m_socket->errorString();
            }
            return QString::fromUtf8(buffer).trimmed();
        }

        // Accumulate raw bytes and decode once at the end: a multi-byte UTF-8
        // sequence in a tag can straddle two TCP reads, and decoding each chunk
        // on its own would corrupt the split character into U+FFFD.
        buffer += m_socket->readAll();
        // A successful reply ends with "OK\n"; an error is a single
        // "ACK [code@idx] {cmd} message\n" line. When that ACK is the first
        // line it has no preceding newline, so match the start too — otherwise
        // the loop blocks until the 5s timeout and reports a bogus socket error.
        if (buffer.endsWith("OK\n") || buffer.startsWith("ACK ") || buffer.contains("\nACK ")) {
            break;
        }
    }

    QString response = QString::fromUtf8(buffer);
    if ((response.startsWith(QStringLiteral("ACK ")) || response.contains(QStringLiteral("\nACK ")))
        && error != nullptr) {
        *error = response.trimmed();
    }
    if (response.endsWith(QStringLiteral("OK\n"))) {
        response.chop(3);
    }
    return response.trimmed();
}
