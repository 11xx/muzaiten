#pragma once

// PipeWire snapshots for direct-device tests, served through
// AudioDeviceControl's tooling seam: no pw-dump or wpctl runs, and a profile
// switch is only recorded, so no real card is ever touched.

#include "playback/AudioDeviceControl.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>

struct FakeCard {
    int pwId = 0;
    int cardIndex = 0;
    QString stableId;
    QString description;
    bool heldByPipeWire = true;  // current profile is the audio one, not "off"
    bool hasSink = true;
};

class FakeAudioGraph {
public:
    FakeAudioGraph()
    {
        m_previous = AudioDeviceControl::setTooling({
            [] { return true; },
            [this]() -> std::optional<QByteArray> {
                ++dumps;
                return QJsonDocument(snapshot()).toJson(QJsonDocument::Compact);
            },
            [this](int pwId, int profileIndex, QString *) {
                profileSwitches.append({pwId, profileIndex});
                return true;
            },
        });
    }
    ~FakeAudioGraph() { AudioDeviceControl::setTooling(m_previous); }
    FakeAudioGraph(const FakeAudioGraph &) = delete;
    FakeAudioGraph &operator=(const FakeAudioGraph &) = delete;

    QJsonArray snapshot() const
    {
        QJsonArray objects;
        for (const FakeCard &card : cards) {
            const QString hw = QStringLiteral("hw:%1").arg(card.cardIndex);
            const QJsonObject props{
                {QStringLiteral("device.api"), QStringLiteral("alsa")},
                {QStringLiteral("api.alsa.path"), hw},
                {QStringLiteral("api.alsa.card"), QString::number(card.cardIndex)},
                {QStringLiteral("device.name"), card.stableId},
                {QStringLiteral("device.description"), card.description},
            };
            const QJsonArray profiles{
                QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("name"), QStringLiteral("off")},
                            {QStringLiteral("available"), QStringLiteral("yes")}, {QStringLiteral("priority"), 0}},
                QJsonObject{{QStringLiteral("index"), 1}, {QStringLiteral("name"), QStringLiteral("output:analog-stereo")},
                            {QStringLiteral("available"), QStringLiteral("yes")}, {QStringLiteral("priority"), 100}},
            };
            const int current = card.heldByPipeWire ? 1 : 0;
            const QJsonObject params{
                {QStringLiteral("Profile"),
                 QJsonArray{QJsonObject{{QStringLiteral("index"), current},
                                        {QStringLiteral("name"), current == 0 ? QStringLiteral("off")
                                                                              : QStringLiteral("output:analog-stereo")}}}},
                {QStringLiteral("EnumProfile"), profiles},
            };
            objects.append(QJsonObject{
                {QStringLiteral("id"), card.pwId},
                {QStringLiteral("type"), QStringLiteral("PipeWire:Interface:Device")},
                {QStringLiteral("info"), QJsonObject{{QStringLiteral("props"), props}, {QStringLiteral("params"), params}}},
            });
            if (card.hasSink) {
                objects.append(QJsonObject{
                    {QStringLiteral("id"), card.pwId + 1000},
                    {QStringLiteral("type"), QStringLiteral("PipeWire:Interface:Node")},
                    {QStringLiteral("info"),
                     QJsonObject{{QStringLiteral("props"),
                                  QJsonObject{{QStringLiteral("media.class"), QStringLiteral("Audio/Sink")},
                                              {QStringLiteral("device.id"), QString::number(card.pwId)},
                                              {QStringLiteral("node.name"),
                                               QStringLiteral("alsa_output.") + card.stableId}}}}},
                });
            }
        }
        return objects;
    }

    QVector<FakeCard> cards;
    QVector<QPair<int, int>> profileSwitches;  // {PipeWire id, profile index}
    int dumps = 0;

private:
    AudioDeviceControl::Tooling m_previous;
};
