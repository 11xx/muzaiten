#pragma once

#include <QDialog>

class QComboBox;
class QSpinBox;

class StopAfterDialog final : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Minutes, Songs };
    Q_ENUM(Mode)

    explicit StopAfterDialog(QWidget *parent = nullptr);

    Mode mode() const;
    int value() const;

private:
    QComboBox *m_condition = nullptr;
    QSpinBox *m_value = nullptr;
};
