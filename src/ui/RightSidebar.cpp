#include "ui/RightSidebar.h"

#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

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

    connect(m_queueTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        emit queueTrackActivated(row);
    });

    m_albumArt = new QLabel(this);
    m_albumArt->setMinimumSize(220, 220);
    m_albumArt->setMaximumHeight(360);
    m_albumArt->setAlignment(Qt::AlignCenter);
    m_albumArt->setFrameShape(QFrame::StyledPanel);
    m_albumArt->setText(QStringLiteral("Album art"));
    m_albumArt->setScaledContents(false);
    layout->addWidget(m_albumArt, 0);
}

void RightSidebar::setQueue(const QVector<Track> &tracks)
{
    m_queueTable->setRowCount(0);
    for (int row = 0; row < tracks.size(); ++row) {
        const Track &track = tracks.at(row);
        m_queueTable->insertRow(row);
        m_queueTable->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
        m_queueTable->setItem(row, 1, new QTableWidgetItem(track.title));
        m_queueTable->setItem(row, 2, new QTableWidgetItem(track.rating0To100 >= 0 ? QString::number(track.rating0To100) : QStringLiteral("-")));
    }
}

void RightSidebar::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_queueTable->rowCount()) {
        m_queueTable->clearSelection();
        return;
    }
    m_queueTable->selectRow(index);
    m_queueTable->scrollToItem(m_queueTable->item(index, 1), QAbstractItemView::PositionAtCenter);
}

void RightSidebar::setAlbumArt(const QString &imagePath)
{
    QPixmap pixmap(imagePath);
    if (pixmap.isNull()) {
        m_albumArt->setPixmap({});
        m_albumArt->setText(QStringLiteral("Album art"));
        return;
    }

    m_albumArt->setText({});
    const int side = std::min(m_albumArt->width(), std::max(220, m_albumArt->height()));
    m_albumArt->setPixmap(pixmap.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
