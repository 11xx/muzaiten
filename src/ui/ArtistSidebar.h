#pragma once

#include "search/SearchMatcher.h"

#include <QWidget>

#include <QString>
#include <QVector>

class Artist;
class QListView;
class QStandardItemModel;
class QTabBar;

class ArtistSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit ArtistSidebar(QWidget *parent = nullptr);

    void setArtists(const QVector<Artist> &artists);
    // reveal=false selects without scrolling the item into view, so a background
    // refresh can re-establish selection without disturbing the user's scroll.
    bool selectArtist(const QString &artistName, bool reveal = true);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    void setMpdAvailable(bool available);
    void setLibrarySourceIndex(int index);
    QWidget *navigationWidget() const;
    void setNavigationScrollPadding(int rows);
    int rowCount() const;
    int currentRow() const;
    QString currentArtistName() const;
    void setCurrentRow(int row);
    void setCurrentRow(int row, int scrollDirection);
    void moveCurrentRow(int delta, bool activate);
    void activateCurrentArtist();
    QVector<Search::MatchDocument> searchDocuments() const;

signals:
    void artistSelected(const QString &artistName);
    void viewSettingsChanged();
    void librarySourceChanged(int index);

private:
    void showContextMenu(const QPoint &pos);
    void applyRowHeight();
    void scrollRowToNavigationAnchor(int row, int direction, int previousTopRow);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void restyleChrome();
    QTabBar *m_tabBar = nullptr;
    QListView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    bool m_showAlbumCount = true;
    int m_rowHeight = 18;
    int m_navigationScrollPadding = 3;
    bool m_mpdAvailable = false;
};
