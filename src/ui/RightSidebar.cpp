#include "ui/RightSidebar.h"

#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

RightSidebar::RightSidebar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_queueTable = new QTableWidget(0, 3, this);
    m_queueTable->setHorizontalHeaderLabels({
        QStringLiteral("#"),
        QStringLiteral("Title"),
        QStringLiteral("Rating"),
    });
    m_queueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queueTable->verticalHeader()->setVisible(false);
    m_queueTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_queueTable->setAlternatingRowColors(true);
    layout->addWidget(m_queueTable, 1);

    m_albumArt = new QLabel(this);
    m_albumArt->setMinimumSize(220, 220);
    m_albumArt->setMaximumHeight(360);
    m_albumArt->setAlignment(Qt::AlignCenter);
    m_albumArt->setFrameShape(QFrame::StyledPanel);
    m_albumArt->setText(QStringLiteral("Album art"));
    m_albumArt->setScaledContents(false);
    layout->addWidget(m_albumArt, 0);
}

