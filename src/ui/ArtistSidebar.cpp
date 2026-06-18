#include "ui/ArtistSidebar.h"

#include "core/Artist.h"
#include "ui/OverlayScrollBar.h"
#include "ui/PanelBorderStyle.h"
#include "ui/SelectionColors.h"

#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
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
            painter->fillRect(opt.rect, SelectionColors::selectedFill(opt));
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
        painter->setPen(selected ? SelectionColors::selectedText(option) : option.palette.color(QPalette::Text));
        QRect nameRect = textRect;
        if (showCount && countValue.isValid()) {
            const QString count = QString::number(countValue.toInt());
            const int countWidth = option.fontMetrics.horizontalAdvance(count) + 8;
            nameRect.setRight(textRect.right() - countWidth);
            painter->setPen(selected ? SelectionColors::selectedText(option) : option.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, count);
            painter->setPen(selected ? SelectionColors::selectedText(option) : option.palette.color(QPalette::Text));
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
    layout->setContentsMargins(6, 0, 6, 6);
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
        "  border: 1px solid %1;"
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
        "}").arg(panelSeparatorColorCss(m_tabBar)));
    layout->addWidget(m_tabBar);

    connect(m_tabBar, &QTabBar::currentChanged, this, &ArtistSidebar::librarySourceChanged);

    m_model = new QStandardItemModel(this);
    m_model->appendRow(new QStandardItem(QStringLiteral("Pick a library folder")));

    m_view = new QListView(this);
    m_view->setObjectName(QStringLiteral("ArtistList"));
    m_view->setModel(m_model);
    m_view->setItemDelegate(new ArtistSidebarDelegate(this));
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setStyleSheet(panelBorderStyleSheet(
        QStringLiteral("QListView#ArtistList"), panelAllBorders(), m_view, QStringLiteral(" border-radius: 3px;")));
    m_view->setUniformItemSizes(true);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
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
    // Preserve the user's selection + scroll across the clear/repopulate, so a
    // background refresh (the metadata fill re-runs this often) doesn't reset the
    // list to the top or yank it back to the selected artist.
    const QModelIndex previousIndex = m_view->currentIndex();
    const QString previousArtist = previousIndex.isValid() ? previousIndex.data(Qt::UserRole).toString() : QString();
    const int previousScroll = m_view->verticalScrollBar()->value();

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

    if (!previousArtist.isEmpty() && selectArtist(previousArtist, /*reveal=*/false)) {
        m_view->verticalScrollBar()->setValue(previousScroll);
    } else if (m_model->rowCount() > 0 && !m_view->currentIndex().isValid()) {
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

bool ArtistSidebar::selectArtist(const QString &artistName, bool reveal)
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
        if (reveal) {
            m_view->scrollTo(index, QAbstractItemView::EnsureVisible);
        }
        return true;
    }
    return false;
}

QWidget *ArtistSidebar::navigationWidget() const
{
    return m_view;
}

void ArtistSidebar::setNavigationScrollPadding(int rows)
{
    m_navigationScrollPadding = std::max(0, rows);
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

QString ArtistSidebar::currentArtistName() const
{
    const QModelIndex index = m_view != nullptr ? m_view->currentIndex() : QModelIndex();
    return index.isValid() ? index.data(Qt::UserRole).toString() : QString();
}

void ArtistSidebar::setCurrentRow(int row)
{
    setCurrentRow(row, 0);
}

void ArtistSidebar::setCurrentRow(int row, int scrollDirection)
{
    if (m_model == nullptr || m_view == nullptr || m_model->rowCount() == 0) {
        return;
    }
    const int safeRow = std::clamp(row, 0, m_model->rowCount() - 1);
    const QModelIndex index = m_model->index(safeRow, 0);
    const int previousTopRow = m_view->verticalScrollBar() != nullptr ? m_view->verticalScrollBar()->value() : 0;
    m_view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_view->setCurrentIndex(index);
    scrollRowToNavigationAnchor(safeRow, scrollDirection, previousTopRow);
}

void ArtistSidebar::moveCurrentRow(int delta, bool activate)
{
    if (rowCount() == 0) {
        return;
    }
    const int row = currentRow() >= 0 ? currentRow() : 0;
    setCurrentRow(std::clamp(row + delta, 0, rowCount() - 1), delta);
    if (activate) {
        activateCurrentArtist();
    }
}

void ArtistSidebar::scrollRowToNavigationAnchor(int row, int direction, int previousTopRow)
{
    if (m_model == nullptr || m_view == nullptr || m_view->verticalScrollBar() == nullptr || m_view->viewport() == nullptr) {
        return;
    }

    const int rowHeightPx = std::max(1, m_rowHeight);
    const int visibleRows = std::max(1, m_view->viewport()->height() / rowHeightPx);
    const int padding = std::clamp(m_navigationScrollPadding, 0, std::max(0, visibleRows - 1));
    QScrollBar *bar = m_view->verticalScrollBar();
    const int topRow = std::clamp(previousTopRow, bar->minimum(), bar->maximum());
    int desiredTop = topRow;

    if (direction > 0) {
        const int anchor = visibleRows - 1 - padding;
        if (row - topRow > anchor) {
            desiredTop = row - anchor;
        }
    } else if (direction < 0) {
        const int anchor = padding;
        if (row - topRow < anchor) {
            desiredTop = row - anchor;
        }
    } else {
        if (row < topRow) {
            desiredTop = row;
        } else if (row >= topRow + visibleRows) {
            desiredTop = row - visibleRows + 1;
        }
    }

    desiredTop = std::clamp(desiredTop, bar->minimum(), bar->maximum());
    if (desiredTop != bar->value()) {
        bar->setValue(desiredTop);
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
                Search::makeField(Search::MatchFieldRole::Title, name, 400),
                Search::makeField(Search::MatchFieldRole::Artist, name, 300),
                Search::makeField(Search::MatchFieldRole::Free, name, 100),
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

void ArtistSidebar::resetViewSettings()
{
    m_showAlbumCount = true;
    m_rowHeight = 18;
    applyRowHeight();
    emit viewSettingsChanged();
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
    menu.addSeparator();
    QAction *reset = menu.addAction(QStringLiteral("Reset sidebar layout to defaults"));
    connect(reset, &QAction::triggered, this, &ArtistSidebar::resetViewSettings);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
