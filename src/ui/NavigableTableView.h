#pragma once

#include <QTableView>

class NavigableTableView : public QTableView {
    Q_OBJECT

public:
    explicit NavigableTableView(QWidget *parent = nullptr);

    void refreshTheme();

    void setNavigationScrollPadding(int rows);
    int navigationScrollPadding() const;

    int currentNavigationRow() const;
    void setCurrentNavigationRow(int row, int direction = 0);

    void setMainPanelActive(bool active);
    bool mainPanelActive() const;

signals:
    void navigationRowChanged(int row);

protected:
    void rowsInserted(const QModelIndex &parent, int start, int end) override;
    void rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;
    void changeEvent(QEvent *event) override;

private:
    void scrollNavigationRowToAnchor(int row, int direction, int previousTopRow);
    void updateRow(const QModelIndex &index);
    void refreshInactiveHighlight();
    void refreshWidgetTheme(QWidget *widget);

    int m_navigationScrollPadding = 3;
    bool m_mainPanelActive = false;
    bool m_refreshingPalette = false;
};
