#pragma once

#include <QWidget>

class QListView;
class QLineEdit;
class QStandardItemModel;

class ArtistSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit ArtistSidebar(QWidget *parent = nullptr);

private:
    QLineEdit *m_filter = nullptr;
    QListView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
};

