#pragma once

#include "ui/trackinfo/TrackInfoSettings.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QSpinBox;

// A small polymorphic editor for a single "Only if ..." threshold: a prompt
// label plus either an integer spinbox or a colon-masked clock field, swapped in
// by kind. Greys out when no value-bearing condition applies. Reusable by any
// settings UI that edits one conditional numeric/duration value.
class ConditionEditor final : public QWidget {
    Q_OBJECT

public:
    explicit ConditionEditor(QWidget *parent = nullptr);

    // Configure for a condition. Kind::None greys the editor out.
    void setCondition(trackinfo::ConditionEditorKind kind,
                      const QString &prompt,
                      int minValue,
                      int maxValue,
                      const QString &suffix);

    // Canonical value: the spin value, or milliseconds for a duration.
    int value() const;
    void setValue(int value);

signals:
    void valueChanged();

private:
    QLabel *m_prompt = nullptr;
    QSpinBox *m_spin = nullptr;
    QLineEdit *m_time = nullptr;
    trackinfo::ConditionEditorKind m_kind = trackinfo::ConditionEditorKind::None;
};
