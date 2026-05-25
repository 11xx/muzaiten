#pragma once

#include <QWidget>

#include <QString>
#include <QVector>

class Artist;
class QListView;
class QLineEdit;
class QStandardItemModel;
class QTabBar;

class ArtistSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit ArtistSidebar(QWidget *parent = nullptr);

    void setArtists(const QVector<Artist> &artists);
    bool selectArtist(const QString &artistName);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void setMpdAvailable(bool available);
    void setLibrarySourceIndex(int index);

signals:
    void artistSelected(const QString &artistName);
    void viewSettingsChanged();
    void librarySourceChanged(int index);

private:
    void showContextMenu(const QPoint &pos);
    void applyRowHeight();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QTabBar *m_tabBar = nullptr;
    QLineEdit *m_filter = nullptr;
    QListView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    bool m_showAlbumCount = true;
    int m_rowHeight = 18;
    bool m_mpdAvailable = false;
};
