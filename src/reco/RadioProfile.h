#pragma once

#include "reco/TrackScorer.h"

#include <QString>
#include <QVector>

struct RadioProfile {
    QString name;
    QString createdAtUtc;
    QString modifiedAtUtc;
    TrackScorer::Weights weights = TrackScorer::defaultWeights();
    TrackScorer::RadioSessionDecay sessionDecay = TrackScorer::defaultSessionDecay();
};

bool operator==(const RadioProfile &left, const RadioProfile &right);
bool operator!=(const RadioProfile &left, const RadioProfile &right);

class RadioProfileStore final {
public:
    RadioProfileStore();

    bool load();
    bool save() const;

    const QVector<RadioProfile> &profiles() const;
    QString activeProfileName() const;
    const RadioProfile &activeProfile() const;
    bool setProfiles(QVector<RadioProfile> profiles, const QString &activeProfileName);
    bool setActiveProfileName(const QString &name);

    static RadioProfile defaultProfile();
    static QString storagePath();

private:
    void restoreFactoryDefaults();

    QVector<RadioProfile> m_profiles;
    QString m_activeProfileName;
};
