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
    bool selectArtist(const QString &artistName);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);

signals:
    void artistSelected(const QString &artistName);
    void viewSettingsChanged();

private:
    void showContextMenu(const QPoint &pos);

private:
    QLineEdit *m_filter = nullptr;
    QListView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    bool m_showAlbumCount = true;
    int m_rowHeight = 22;
};
