#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QComboBox;
class QListWidget;
class QListWidgetItem;
class QSpinBox;
class ResponsiveColumnLayout;

struct ResponsiveColumnOption {
    QString key;
    QString label;
};

class ResponsiveColumnOptionsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ResponsiveColumnOptionsDialog(ResponsiveColumnLayout *layout,
                                           QVector<ResponsiveColumnOption> columns,
                                           QWidget *parent = nullptr);

private:
    void refreshList();
    void updateEditor();
    void moveSelected(int delta);
    QString selectedKey() const;
    QString labelForKey(const QString &key) const;

    ResponsiveColumnLayout *m_layout = nullptr;
    QVector<ResponsiveColumnOption> m_columns;
    QListWidget *m_orderList = nullptr;
    QComboBox *m_priorityCombo = nullptr;
    QSpinBox *m_minWidthSpin = nullptr;
    bool m_updating = false;
};
