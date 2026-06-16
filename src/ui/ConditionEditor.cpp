#include "ui/ConditionEditor.h"

#include "core/HumanQuantity.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>

using trackinfo::ConditionEditorKind;

ConditionEditor::ConditionEditor(QWidget *parent)
    : QWidget(parent)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    m_prompt = new QLabel(this);
    m_spin = new QSpinBox(this);
    m_spin->setMaximumWidth(110);
    m_time = new QLineEdit(this);
    // h:mm:ss with the colons pre-filled and digit-only fields.
    m_time->setInputMask(QStringLiteral("9:99:99"));
    m_time->setMaximumWidth(90);
    row->addWidget(m_prompt);
    row->addWidget(m_spin);
    row->addWidget(m_time);
    row->addStretch(1);
    m_spin->hide();
    m_time->hide();

    connect(m_spin, &QSpinBox::valueChanged, this, [this]() { emit valueChanged(); });
    connect(m_time, &QLineEdit::textEdited, this, [this]() { emit valueChanged(); });
}

void ConditionEditor::setCondition(ConditionEditorKind kind,
                                   const QString &prompt,
                                   int minValue,
                                   int maxValue,
                                   const QString &suffix)
{
    m_kind = kind;
    const bool active = kind != ConditionEditorKind::None;
    m_prompt->setText(active ? prompt : QStringLiteral("Only if condition"));
    m_prompt->setEnabled(active);
    m_spin->setVisible(kind == ConditionEditorKind::IntSpin);
    m_spin->setEnabled(active);
    m_time->setVisible(kind == ConditionEditorKind::Duration);
    m_time->setEnabled(active);
    if (kind == ConditionEditorKind::IntSpin) {
        // Block valueChanged while reconfiguring the range: QSpinBox clamps its
        // current value into the new range and emits valueChanged for any change,
        // which would fire the dialog's "store condValue" slot before setValue()
        // is called, corrupting Qt::UserRole+1 with the clamped value.
        const QSignalBlocker blocker(m_spin);
        m_spin->setRange(minValue, maxValue);
        m_spin->setSuffix(suffix);
    }
}

int ConditionEditor::value() const
{
    if (m_kind == ConditionEditorKind::Duration) {
        // text() strips mask separators (colons) from a masked QLineEdit, returning
        // e.g. "00500" for "0:05:00". Use displayText() which preserves them so
        // parseDuration can split on ':' and parse correctly.
        return static_cast<int>(humanquantity::parseDuration(m_time->displayText()));
    }
    return m_spin->value();
}

void ConditionEditor::setValue(int value)
{
    if (m_kind == ConditionEditorKind::Duration) {
        const QSignalBlocker blocker(m_time);
        const int totalSeconds = std::max(0, value) / 1000;
        m_time->setText(QStringLiteral("%1:%2:%3")
                            .arg(totalSeconds / 3600)
                            .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
                            .arg(totalSeconds % 60, 2, 10, QLatin1Char('0')));
        return;
    }
    const QSignalBlocker blocker(m_spin);
    m_spin->setValue(value);
}
