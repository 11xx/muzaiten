#pragma once

#include <QWidget>

#include <QString>
#include <QVector>

class Artist;
class QListView;
class QLineEdit;
class QStandardItemModel;

class ArtistSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit ArtistSidebar(QWidget *parent = nullptr);

    void setArtists(const QVector<Artist> &artists);

signals:
    void artistSelected(const QString &artistName);

private:
    QLineEdit *m_filter = nullptr;
    QListView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
};
