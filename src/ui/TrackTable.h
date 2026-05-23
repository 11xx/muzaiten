#pragma once

#include <QTableView>

class TrackTable final : public QTableView {
    Q_OBJECT

public:
    explicit TrackTable(QWidget *parent = nullptr);
};

