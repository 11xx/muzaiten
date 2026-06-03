#include "ui/ResponsiveColumnOptionsDialog.h"

#include "ui/ResponsiveColumnLayout.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString priorityLabel(ResponsiveColumnPriority priority)
{
    switch (priority) {
    case ResponsiveColumnPriority::Keep:
        return QStringLiteral("Keep");
    case ResponsiveColumnPriority::Normal:
        return QStringLiteral("Hide later");
    case ResponsiveColumnPriority::HideEarly:
        return QStringLiteral("Hide early");
    }
    return QStringLiteral("Hide later");
}

} // namespace

ResponsiveColumnOptionsDialog::ResponsiveColumnOptionsDialog(ResponsiveColumnLayout *layout,
                                                             QVector<ResponsiveColumnOption> columns,
                                                             QWidget *parent)
    : QDialog(parent)
    , m_layout(layout)
    , m_columns(std::move(columns))
{
    setWindowTitle(QStringLiteral("Responsive column options"));
    resize(420, 360);

    auto *root = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    root->addLayout(body, 1);

    m_orderList = new QListWidget(this);
    body->addWidget(m_orderList, 1);

    auto *side = new QVBoxLayout;
    body->addLayout(side);

    auto *upButton = new QPushButton(QStringLiteral("Move up"), this);
    auto *downButton = new QPushButton(QStringLiteral("Move down"), this);
    side->addWidget(upButton);
    side->addWidget(downButton);

    auto *form = new QFormLayout;
    m_priorityCombo = new QComboBox(this);
    m_priorityCombo->addItem(priorityLabel(ResponsiveColumnPriority::Keep), static_cast<int>(ResponsiveColumnPriority::Keep));
    m_priorityCombo->addItem(priorityLabel(ResponsiveColumnPriority::Normal), static_cast<int>(ResponsiveColumnPriority::Normal));
    m_priorityCombo->addItem(priorityLabel(ResponsiveColumnPriority::HideEarly), static_cast<int>(ResponsiveColumnPriority::HideEarly));
    form->addRow(QStringLiteral("Drop behavior"), m_priorityCombo);

    m_minWidthSpin = new QSpinBox(this);
    m_minWidthSpin->setRange(1, 2000);
    m_minWidthSpin->setSuffix(QStringLiteral(" px"));
    form->addRow(QStringLiteral("Minimum width"), m_minWidthSpin);
    side->addLayout(form);
    side->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(upButton, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
    connect(downButton, &QPushButton::clicked, this, [this]() { moveSelected(1); });
    connect(m_orderList, &QListWidget::currentItemChanged, this, [this]() { updateEditor(); });
    connect(m_priorityCombo, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updating || m_layout == nullptr) {
            return;
        }
        const QString key = selectedKey();
        if (!key.isEmpty()) {
            m_layout->setColumnPriority(key, static_cast<ResponsiveColumnPriority>(m_priorityCombo->currentData().toInt()));
        }
    });
    connect(m_minWidthSpin, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_updating || m_layout == nullptr) {
            return;
        }
        const QString key = selectedKey();
        if (!key.isEmpty()) {
            m_layout->setColumnMinimumWidth(key, value);
        }
    });

    refreshList();
    if (m_orderList->count() > 0) {
        m_orderList->setCurrentRow(0);
    }
}

void ResponsiveColumnOptionsDialog::refreshList()
{
    if (m_layout == nullptr) {
        return;
    }
    const QString selected = selectedKey();
    m_orderList->clear();
    for (const QString &key : m_layout->dropOrderKeys()) {
        auto *item = new QListWidgetItem(labelForKey(key));
        item->setData(Qt::UserRole, key);
        m_orderList->addItem(item);
        if (key == selected) {
            m_orderList->setCurrentItem(item);
        }
    }
}

void ResponsiveColumnOptionsDialog::updateEditor()
{
    if (m_layout == nullptr) {
        return;
    }
    const QString key = selectedKey();
    const bool hasSelection = !key.isEmpty();
    m_updating = true;
    m_priorityCombo->setEnabled(hasSelection);
    m_minWidthSpin->setEnabled(hasSelection);
    if (hasSelection) {
        const int priorityIndex = m_priorityCombo->findData(static_cast<int>(m_layout->columnPriority(key)));
        m_priorityCombo->setCurrentIndex(std::max(0, priorityIndex));
        m_minWidthSpin->setMinimum(m_layout->defaultColumnMinimumWidth(key));
        m_minWidthSpin->setValue(m_layout->columnMinimumWidth(key));
    }
    m_updating = false;
}

void ResponsiveColumnOptionsDialog::moveSelected(int delta)
{
    if (m_layout == nullptr) {
        return;
    }
    const int row = m_orderList->currentRow();
    const int destination = row + delta;
    if (row < 0 || destination < 0 || destination >= m_orderList->count()) {
        return;
    }

    QStringList keys = m_layout->dropOrderKeys();
    keys.move(row, destination);
    m_layout->setDropOrderKeys(keys);
    refreshList();
    m_orderList->setCurrentRow(destination);
}

QString ResponsiveColumnOptionsDialog::selectedKey() const
{
    if (QListWidgetItem *item = m_orderList->currentItem()) {
        return item->data(Qt::UserRole).toString();
    }
    return {};
}

QString ResponsiveColumnOptionsDialog::labelForKey(const QString &key) const
{
    for (const ResponsiveColumnOption &column : m_columns) {
        if (column.key == key) {
            return column.label;
        }
    }
    return key;
}
