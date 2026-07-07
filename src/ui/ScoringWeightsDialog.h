#pragma once

#include "reco/WeightLearner.h"
#include "reco/WeightLearnerData.h"
#include "reco/TrackScorer.h"

#include <QDialog>
#include <QHash>

class Database;
class QLabel;
class QListWidget;
class QDoubleSpinBox;

class ScoringWeightsDialog final : public QDialog {
    Q_OBJECT

public:
    ScoringWeightsDialog(Database *db, QString historyPath, QWidget *parent = nullptr);

private:
    void loadActiveWeights();
    void applyCurrentWeights();
    void resetDefaults();
    void refreshProfiles();
    void applySelectedProfile();
    void saveCurrentAsProfile();
    void deleteSelectedProfile();
    void suggestFromListening();
    void showLearnedWeights(const WeightLearner::Result &learned,
                            const WeightLearnerData::LoadResult &load);
    TrackScorer::Weights currentWeights() const;
    QByteArray currentWeightsJson() const;
    QString selectedProfileName() const;
    void showError(const QString &message);
    void setStatus(const QString &message);

    Database *m_db = nullptr;
    QString m_historyPath;
    QLabel *m_status = nullptr;
    QListWidget *m_profiles = nullptr;
    QHash<QString, QDoubleSpinBox *> m_spins;
};
