#include "ui/ArtistSidebar.h"

#include "core/Artist.h"

#include <QLineEdit>
#include <QListView>
#include <QStandardItemModel>
#include <QVBoxLayout>

ArtistSidebar::ArtistSidebar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Filter album artists"));
    layout->addWidget(m_filter);

    m_model = new QStandardItemModel(this);
    m_model->appendRow(new QStandardItem(QStringLiteral("Pick a library folder")));

    m_view = new QListView(this);
    m_view->setModel(m_model);
    layout->addWidget(m_view, 1);

    connect(m_view, &QListView::clicked, this, [this](const QModelIndex &index) {
        emit artistSelected(index.data(Qt::UserRole).toString());
    });
}

void ArtistSidebar::setArtists(const QVector<Artist> &artists)
{
    m_model->clear();
    for (const Artist &artist : artists) {
        const QString label = QStringLiteral("%1  (%2)").arg(artist.name).arg(artist.albumCount);
        auto *item = new QStandardItem(label);
        item->setEditable(false);
        item->setData(artist.name, Qt::UserRole);
        m_model->appendRow(item);
    }
}
