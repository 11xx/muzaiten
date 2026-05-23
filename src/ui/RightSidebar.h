#pragma once

#include <QWidget>

class QLabel;
class QTableWidget;

class RightSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit RightSidebar(QWidget *parent = nullptr);

private:
    QTableWidget *m_queueTable = nullptr;
    QLabel *m_albumArt = nullptr;
};

