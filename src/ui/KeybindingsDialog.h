#pragma once

#include "ui/KeybindCatalog.h"

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QString>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QWidget;

// Settings > Keybinds: one page per screen. The Queue, Library and File
// explorer pages edit their keys over a chosen preset; the other pages list
// fixed keys.
class KeybindingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit KeybindingsDialog(QWidget *parent = nullptr);
    ~KeybindingsDialog() override;

    void setMainPanelProfileName(const QString &name);
    QString mainPanelProfileName() const;
    void setFileExplorerProfileName(const QString &name);
    QString fileExplorerProfileName() const;
    void setQueueProfileName(const QString &name);
    QString queueProfileName() const;
    void setFileExplorerKeyHintsVisible(bool visible);
    bool fileExplorerKeyHintsVisible() const;

    void setOverrides(const QString &scopeId, const KeyBindingOverrides &overrides);
    KeyBindingOverrides overrides(const QString &scopeId) const;
    KeyBindingMap effectiveBindings(const QString &scopeId) const;

    enum class KeyRequest { Assigned, Conflict, Reserved, AlreadyAssigned };
    // Offers a key for an action on a screen. A key another action has waits
    // for confirmUseHere() or keepElsewhere().
    KeyRequest requestKey(const QString &scopeId, const QString &action, const QKeySequence &key);
    void confirmUseHere(const QString &scopeId);
    void keepElsewhere(const QString &scopeId);
    void removeKey(const QString &scopeId, const QString &action, const QKeySequence &key);
    void resetAction(const QString &scopeId, const QString &action);
    void restoreDefaults(const QString &scopeId);
    // Shows only the actions a key runs on the current screen; an empty key
    // shows every action again.
    void lookUpKey(const QKeySequence &key);
    void selectScreen(const QString &id);
    void selectAction(const QString &scopeId, const QString &action);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Page;

    Page *page(const QString &scopeId) const;
    Page *currentPage() const;
    QWidget *buildEditablePage(Page &page);
    QWidget *buildFixedPage(const QString &title, const KeyBindingReferenceList &rows);
    void refreshTree(Page &page);
    void refreshEditor(Page &page);
    void applyFilter(Page &page);
    void setOverride(Page &page, const QString &action, const QList<QKeySequence> &keys);
    void startCapture(Page &page);
    void finishCapture(Page &page);
    void setFinding(bool finding);
    void updateFooter();

    QListWidget *m_screens = nullptr;
    QStackedWidget *m_pages = nullptr;
    QPushButton *m_restoreDefaults = nullptr;
    QVector<Page *> m_editablePages;
    QStringList m_screenIds;
    bool m_finding = false;
};
