#include "ui/ArtistSidebar.h"

#include "core/Artist.h"

#include <QAction>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

namespace {

class ArtistSidebarDelegate final : public QStyledItemDelegate {
public:
    explicit ArtistSidebarDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        return index.data(Qt::SizeHintRole).toSize().isValid() ? index.data(Qt::SizeHintRole).toSize() : QSize(120, 20);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        QStyle *style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QRect textRect = option.rect.adjusted(6, 0, -6, 0);
        const QString name = index.data(Qt::UserRole).toString();
        const QVariant countValue = index.data(Qt::UserRole + 1);
        const bool showCount = index.data(Qt::UserRole + 2).toBool();

        painter->save();
        painter->setPen(option.palette.color(QPalette::Text));
        QRect nameRect = textRect;
        if (showCount && countValue.isValid()) {
            const QString count = QString::number(countValue.toInt());
            const int countWidth = option.fontMetrics.horizontalAdvance(count) + 8;
            nameRect.setRight(textRect.right() - countWidth);
            painter->setPen(option.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, count);
            painter->setPen(option.palette.color(QPalette::Text));
        }
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, option.fontMetrics.elidedText(name, Qt::ElideRight, nameRect.width()));
        painter->restore();
    }
};

} // namespace

ArtistSidebar::ArtistSidebar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(5);

    m_tabBar = new QTabBar(this);
    m_tabBar->addTab(QStringLiteral("Library"));
    m_tabBar->addTab(QStringLiteral("MPD"));
    m_tabBar->setExpanding(false);
    m_tabBar->setTabEnabled(1, false);
    layout->addWidget(m_tabBar);

    connect(m_tabBar, &QTabBar::currentChanged, this, &ArtistSidebar::librarySourceChanged);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Filter album artists"));
    layout->addWidget(m_filter);

    m_model = new QStandardItemModel(this);
    m_model->appendRow(new QStandardItem(QStringLiteral("Pick a library folder")));

    m_view = new QListView(this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(new ArtistSidebarDelegate(this));
    m_view->setUniformItemSizes(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->viewport()->installEventFilter(this);
    layout->addWidget(m_view, 1);

    connect(m_view, &QListView::clicked, this, [this](const QModelIndex &index) {
        emit artistSelected(index.data(Qt::UserRole).toString());
    });
    connect(m_view, &QListView::customContextMenuRequested, this, &ArtistSidebar::showContextMenu);
}

void ArtistSidebar::setArtists(const QVector<Artist> &artists)
{
    m_model->clear();
    for (const Artist &artist : artists) {
        auto *item = new QStandardItem(artist.name);
        item->setEditable(false);
        item->setData(artist.name, Qt::UserRole);
        item->setData(artist.albumCount, Qt::UserRole + 1);
        item->setData(m_showAlbumCount, Qt::UserRole + 2);
        m_model->appendRow(item);
    }
    applyRowHeight();
}

void ArtistSidebar::setMpdAvailable(bool available)
{
    m_mpdAvailable = available;
    m_tabBar->setTabEnabled(1, available);
    if (!available && m_tabBar->currentIndex() == 1) {
        m_tabBar->setCurrentIndex(0);
    }
}

void ArtistSidebar::setLibrarySourceIndex(int index)
{
    const int safeIndex = (index == 1 && m_mpdAvailable) ? 1 : 0;
    const QSignalBlocker blocker(m_tabBar);
    m_tabBar->setCurrentIndex(safeIndex);
}

bool ArtistSidebar::selectArtist(const QString &artistName)
{
    if (artistName.isEmpty()) {
        return false;
    }

    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex index = m_model->index(row, 0);
        if (index.data(Qt::UserRole).toString() != artistName) {
            continue;
        }

        const QSignalBlocker blocker(m_view->selectionModel());
        m_view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_view->setCurrentIndex(index);
        m_view->scrollTo(index, QAbstractItemView::PositionAtCenter);
        return true;
    }
    return false;
}

QString ArtistSidebar::viewSettingsJson() const
{
    QJsonObject root;
    root.insert(QStringLiteral("showAlbumCount"), m_showAlbumCount);
    root.insert(QStringLiteral("rowHeight"), m_rowHeight);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void ArtistSidebar::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    m_showAlbumCount = root.value(QStringLiteral("showAlbumCount")).toBool(true);
    m_rowHeight = std::clamp(root.value(QStringLiteral("rowHeight")).toInt(20), 18, 40);
    applyRowHeight();
}

void ArtistSidebar::applyRowHeight()
{
    for (int row = 0; row < m_model->rowCount(); ++row) {
        m_model->item(row)->setData(m_showAlbumCount, Qt::UserRole + 2);
        m_model->item(row)->setData(QSize(120, m_rowHeight), Qt::SizeHintRole);
    }
    if (m_view != nullptr) {
        m_view->viewport()->update();
    }
}

bool ArtistSidebar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            const int step = wheel->angleDelta().y() > 0 ? 2 : -2;
            m_rowHeight = std::clamp(m_rowHeight + step, 18, 40);
            applyRowHeight();
            emit viewSettingsChanged();
            wheel->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ArtistSidebar::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    QAction *showCount = menu.addAction(QStringLiteral("Show album count"));
    showCount->setCheckable(true);
    showCount->setChecked(m_showAlbumCount);
    connect(showCount, &QAction::toggled, this, [this](bool checked) {
        m_showAlbumCount = checked;
        applyRowHeight();
        emit viewSettingsChanged();
    });
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
