#pragma once

#include "reco/RadioProfile.h"

#include <QDialog>
#include <QHash>

class AppCore;
class QComboBox;
class QDoubleSpinBox;
class QRadioButton;
class QPushButton;
class QSpinBox;
class QTimer;

class RadioCustomizationDialog final : public QDialog {
    Q_OBJECT

public:
    explicit RadioCustomizationDialog(AppCore *core, QWidget *parent = nullptr);

private:
    void reloadProfile();
    RadioProfile editedProfile() const;
    void previewEdits();
    void flushPreview();
    void updateHistoryButtons();
    void createProfile(bool duplicateActive);
    void renameProfile();

    AppCore *m_core = nullptr;
    QComboBox *m_profiles = nullptr;
    QHash<QString, QDoubleSpinBox *> m_weightControls;
    QSpinBox *m_decayStart = nullptr;
    QRadioButton *m_linear = nullptr;
    QRadioButton *m_exponential = nullptr;
    QDoubleSpinBox *m_noveltyFloor = nullptr;
    QDoubleSpinBox *m_ratingFloor = nullptr;
    QPushButton *m_undo = nullptr;
    QPushButton *m_redo = nullptr;
    QTimer *m_previewTimer = nullptr;
    RadioProfile m_revertBaseline;
    bool m_loading = false;
    bool m_previewDirty = false;
};
