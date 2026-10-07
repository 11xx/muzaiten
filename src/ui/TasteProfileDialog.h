#pragma once

#include <QDialog>

class QComboBox;

class TasteProfileDialog final : public QDialog {
    Q_OBJECT
public:
    explicit TasteProfileDialog(QWidget *parent = nullptr);

signals:
    void startRequested(const QString &name);
    void stopRequested();
    void removed(const QString &name);

private:
    void reload();
    void exportProfile();
    void importProfile();
    void removeProfile();
    QComboBox *m_seeds = nullptr;
};
