#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QTableWidget;

class KeybindingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit KeybindingsDialog(QWidget *parent = nullptr);

    void setMainPanelProfileName(const QString &name);
    QString mainPanelProfileName() const;

    void setFileExplorerProfileName(const QString &name);
    QString fileExplorerProfileName() const;

    void setQueueProfileName(const QString &name);
    QString queueProfileName() const;

    void setFileExplorerKeyHintsVisible(bool visible);
    bool fileExplorerKeyHintsVisible() const;

private:
    static QTableWidget *makeBindingsTable(QWidget *parent);
    void rebuildMainPanelBindings();
    void rebuildFileExplorerBindings();
    void rebuildQueueBindings();

    QComboBox *m_mainPanelProfile = nullptr;
    QTableWidget *m_mainPanelBindings = nullptr;
    QComboBox *m_fileExplorerProfile = nullptr;
    QCheckBox *m_fileExplorerHints = nullptr;
    QTableWidget *m_fileExplorerBindings = nullptr;
    QComboBox *m_queueProfile = nullptr;
    QTableWidget *m_queueBindings = nullptr;
};
