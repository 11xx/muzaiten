#include "ui/ArtistSidebar.h"

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
}

