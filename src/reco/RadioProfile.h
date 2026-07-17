#pragma once

#include "reco/TrackScorer.h"

#include <QString>
#include <QHash>
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
    static constexpr int HistoryLimit = 50;

    struct History {
        QVector<RadioProfile> snapshots;
        int current = 0;
    };
    RadioProfileStore();

    bool load();
    bool save() const;

    const QVector<RadioProfile> &profiles() const;
    QString activeProfileName() const;
    const RadioProfile &activeProfile() const;
    bool setProfiles(QVector<RadioProfile> profiles, const QString &activeProfileName);
    bool setActiveProfileName(const QString &name);

    // Preview changes replace the live profile without creating a persistence
    // boundary. commitActivePreview() adds one immutable undo snapshot once a
    // dialog's debounce timer expires.
    bool previewActiveProfile(const RadioProfile &profile);
    bool commitActivePreview();
    bool restoreActiveProfile(const RadioProfile &profile);
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    bool createProfile(const QString &name, bool duplicateActive);
    bool renameActiveProfile(const QString &name);
    bool deleteActiveProfile();
    bool resetActiveProfile();

    static RadioProfile defaultProfile();
    static QString storagePath();

private:
    RadioProfile *activeProfileMutable();
    const History *activeHistory() const;
    History *activeHistoryMutable();
    void ensureHistory(const QString &name, const RadioProfile &profile);
    void pushActiveSnapshot();
    void restoreFactoryDefaults();

    QVector<RadioProfile> m_profiles;
    QString m_activeProfileName;
    QHash<QString, History> m_histories;
};
