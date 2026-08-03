#include "ui/StopAfterDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>

StopAfterDialog::StopAfterDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Stop after"));

    auto *layout = new QFormLayout(this);

    m_condition = new QComboBox(this);
    m_condition->setObjectName(QStringLiteral("StopAfterCondition"));
    m_condition->addItem(QStringLiteral("Minutes"));
    m_condition->addItem(QStringLiteral("Songs"));
    layout->addRow(QStringLiteral("Condition:"), m_condition);

    m_value = new QSpinBox(this);
    m_value->setObjectName(QStringLiteral("StopAfterValue"));
    m_value->setRange(1, 1440);
    m_value->setValue(60);
    layout->addRow(QStringLiteral("Value:"), m_value);

    connect(m_condition, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index == 0) {
            m_value->setRange(1, 1440);
            m_value->setValue(60);
        } else {
            m_value->setRange(1, 999);
            m_value->setValue(5);
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);
}

StopAfterDialog::Mode StopAfterDialog::mode() const
{
    return m_condition->currentIndex() == 0 ? Mode::Minutes : Mode::Songs;
}

int StopAfterDialog::value() const
{
    return m_value->value();
}
