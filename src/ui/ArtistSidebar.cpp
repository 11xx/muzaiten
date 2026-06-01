#include "ui/ArtistSidebar.h"

#include "core/Artist.h"
#include "ui/OverlayScrollBar.h"

#include <QAction>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
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
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        if (selected) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::Highlight));
        } else if (hovered) {
            QColor hover = opt.palette.color(QPalette::Highlight);
            hover.setAlpha(34);
            painter->fillRect(opt.rect, hover);
        } else if (index.row() % 2 == 1) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
        }

        const QRect textRect = option.rect.adjusted(6, 0, -6, 0);
        const QString name = index.data(Qt::UserRole).toString();
        const QVariant countValue = index.data(Qt::UserRole + 1);
        const bool showCount = index.data(Qt::UserRole + 2).toBool();

        painter->save();
        painter->setPen(selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::Text));
        QRect nameRect = textRect;
        if (showCount && countValue.isValid()) {
            const QString count = QString::number(countValue.toInt());
            const int countWidth = option.fontMetrics.horizontalAdvance(count) + 8;
            nameRect.setRight(textRect.right() - countWidth);
            painter->setPen(selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, count);
            painter->setPen(selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::Text));
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
    m_tabBar->setDocumentMode(true);
    m_tabBar->setDrawBase(false);
    m_tabBar->setFixedHeight(22);
    m_tabBar->setTabEnabled(1, false);
    m_tabBar->setVisible(false);
    m_tabBar->setStyleSheet(QStringLiteral(
        "QTabBar::tab {"
        "  border: 1px solid palette(mid);"
        "  border-bottom: 0;"
        "  border-top-left-radius: 3px;"
        "  border-top-right-radius: 3px;"
        "  padding: 2px 9px;"
        "  margin: 0 1px 0 0;"
        "  min-height: 16px;"
        "  background: palette(button);"
        "}"
        "QTabBar::tab:selected {"
        "  background: palette(base);"
        "  color: palette(text);"
        "}"
        "QTabBar::tab:!selected {"
        "  color: palette(window-text);"
        "}"
        "QTabBar::tab:disabled {"
        "  color: palette(disabled, window-text);"
        "}"));
    layout->addWidget(m_tabBar);

    connect(m_tabBar, &QTabBar::currentChanged, this, &ArtistSidebar::librarySourceChanged);

    m_model = new QStandardItemModel(this);
    m_model->appendRow(new QStandardItem(QStringLiteral("Pick a library folder")));

    m_view = new QListView(this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(new ArtistSidebarDelegate(this));
    m_view->setUniformItemSizes(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->viewport()->installEventFilter(this);
    OverlayScrollBar::install(m_view);
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
    if (m_model->rowCount() > 0 && !m_view->currentIndex().isValid()) {
        setCurrentRow(0);
    }
}

void ArtistSidebar::setMpdAvailable(bool available)
{
    m_mpdAvailable = available;
    m_tabBar->setVisible(available);
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

QWidget *ArtistSidebar::navigationWidget() const
{
    return m_view;
}

int ArtistSidebar::rowCount() const
{
    return m_model != nullptr ? m_model->rowCount() : 0;
}

int ArtistSidebar::currentRow() const
{
    const QModelIndex index = m_view != nullptr ? m_view->currentIndex() : QModelIndex();
    return index.isValid() ? index.row() : -1;
}

void ArtistSidebar::setCurrentRow(int row)
{
    if (m_model == nullptr || m_view == nullptr || m_model->rowCount() == 0) {
        return;
    }
    const int safeRow = std::clamp(row, 0, m_model->rowCount() - 1);
    const QModelIndex index = m_model->index(safeRow, 0);
    m_view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_view->setCurrentIndex(index);
    m_view->scrollTo(index, QAbstractItemView::EnsureVisible);
}

void ArtistSidebar::moveCurrentRow(int delta, bool activate)
{
    if (rowCount() == 0) {
        return;
    }
    const int row = currentRow() >= 0 ? currentRow() : 0;
    setCurrentRow(std::clamp(row + delta, 0, rowCount() - 1));
    if (activate) {
        activateCurrentArtist();
    }
}

void ArtistSidebar::activateCurrentArtist()
{
    const QModelIndex index = m_view != nullptr ? m_view->currentIndex() : QModelIndex();
    if (index.isValid()) {
        emit artistSelected(index.data(Qt::UserRole).toString());
    }
}

QVector<Search::MatchDocument> ArtistSidebar::searchDocuments() const
{
    QVector<Search::MatchDocument> docs;
    if (m_model == nullptr) {
        return docs;
    }
    docs.reserve(m_model->rowCount());
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex index = m_model->index(row, 0);
        const QString name = index.data(Qt::UserRole).toString();
        docs.push_back({
            row,
            {
                {Search::MatchFieldRole::Title, name, name.toLower(), 400},
                {Search::MatchFieldRole::Artist, name, name.toLower(), 300},
                {Search::MatchFieldRole::Free, name, name.toLower(), 100},
            },
            {},
        });
    }
    return docs;
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
    m_rowHeight = std::clamp(root.value(QStringLiteral("rowHeight")).toInt(18), 18, 40);
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
