#pragma once

#include "search/SearchMatcher.h"
#include "ui/MainPanelKeybindings.h"

#include <QHash>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QKeyEvent;

struct MainPanelTarget {
    MainPanelId id = MainPanelId::Artists;
    QString label;
    QWidget *focusWidget = nullptr;

    std::function<int()> rowCount;
    std::function<int()> currentRow;
    std::function<void(int row)> setCurrentRow;
    std::function<void()> activateCurrent;
    std::function<void()> prepareForFocus;
    std::function<void(int horizontal, int vertical)> moveCurrentInGrid;
    std::function<QVector<Search::MatchDocument>()> documents;
};

class PanelSearchController final : public QWidget {
    Q_OBJECT

public:
    explicit PanelSearchController(QWidget *parent = nullptr);

    void registerTarget(MainPanelTarget target);
    void setKeyBindingProfileName(const QString &name);
    QString keyBindingProfileName() const { return m_keyBindingProfileName; }
    void setFocusOrder(const QVector<MainPanelId> &order);
    QVector<MainPanelId> focusOrder() const { return m_focusOrder; }
    void activateForMainView();
    void deactivateForNonMainView();
    void refreshPanel(MainPanelId id);
    void refreshActivePanel();
    MainPanelId activePanel() const { return m_activePanel; }
    void setActivePanel(MainPanelId id, bool focus = true);
    void setActivePanelFromString(const QString &value);

signals:
    void statusMessage(QString message, int timeoutMs);
    void activePanelChanged(MainPanelId id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct PanelState {
        QString query;
        QVector<Search::PanelMatch> matches;
        int currentMatch = -1;
    };

    QWidget *eventWidgetFor(QObject *watched) const;
    MainPanelTarget *targetForId(MainPanelId id);
    const MainPanelTarget *targetForId(MainPanelId id) const;
    MainPanelTarget *targetForWidget(QWidget *widget);
    void updatePanelActiveProperties();
    void focusActivePanel();
    void openSearch();
    void escapeSearch();
    void setQueryForActivePanel(const QString &query);
    void rebuildMatches(MainPanelId id, bool jumpToFirst, bool showNoMatchNotice);
    void updateSearchUi();
    void moveCurrent(int delta);
    void movePage(int direction);
    void focusRelative(int direction);
    void focusQueue();
    void focusTracks();
    void activateCurrent();
    void cycleMatch(int direction);
    bool handleAlbumGridKey(QKeyEvent *event, const QString &action);
    bool handlePanelKey(QKeyEvent *event, MainPanelId panel);
    bool handleSearchKey(QKeyEvent *event);
    int pageStepForActivePanel() const;

    QVector<MainPanelTarget> m_targets;
    QHash<QWidget *, MainPanelId> m_widgetToPanel;
    QHash<MainPanelId, PanelState> m_state;
    QVector<MainPanelId> m_focusOrder;
    KeyBindingMap m_keyBindings;
    QString m_keyBindingProfileName;
    MainPanelId m_activePanel = MainPanelId::Artists;
    bool m_hasActivePanel = false;
    bool m_mainViewActive = false;
    bool m_fuzzyMode = false;

    QLabel *m_prompt = nullptr;
    QLineEdit *m_edit = nullptr;
    QLabel *m_status = nullptr;
};
