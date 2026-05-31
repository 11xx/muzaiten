#include "ui/RightSidebar.h"

#include "ui/AlbumArtFallback.h"
#include "ui/DenseTableDelegate.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/OverlayScrollBar.h"
#include "ui/StarRating.h"
#include "ui/StarRatingDelegate.h"

#include <QAction>
#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDataStream>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QImage>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QSpinBox>
#include <QTableView>
#include <QTableWidget>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QByteArray>
#include <QClipboard>
#include <QFrame>
#include <QWheelEvent>

#include <algorithm>
#include <functional>

namespace {

struct ColumnSpec {
    const char *key;
    const char *label;
    int index;
    int preferredWidth;
    int minWidth = 24;
};

constexpr ColumnSpec columns[] = {
    {"position", "#", 0, 38},
    {"title", "Title", 1, 180},
    {"ratingEdit", "Rating", 2, 96},
    {"rating", "Rating (short)", 3, 56, 12},
    {"artist", "Artist", 4, 120},
    {"album", "Album", 5, 120},
    {"duration", "Duration", 6, 70},
    {"year", "Year", 7, 58},
    {"track", "Track", 8, 36, 12},
};

constexpr auto queueRowsMimeType = "application/x-muzaiten-queue-rows";

QString ratingText(int rating0To100);
QString formatDuration(qint64 durationMs);
QString trackNumberText(const Track &track);

enum QueueRoles {
    TrackRole = Qt::UserRole + 1,
    HoverRatingRole = Qt::UserRole + 2,
    PlayNextOrdinalRole = Qt::UserRole + 3,
};

class QueueTableView final : public QTableView {
public:
    explicit QueueTableView(QWidget *parent = nullptr)
        : QTableView(parent)
    {
    }

    int dropIndicatorRow() const
    {
        return m_dropIndicatorRow;
    }

    void setCurrentPlayingRow(int row)
    {
        if (m_currentPlayingRow == row) {
            return;
        }
        m_currentPlayingRow = row;
        viewport()->update();
    }

    void fitVisibleColumnsToViewport(int fixedColumn = -1)
    {
        if (model() == nullptr || m_fittingColumns) {
            return;
        }

        QVector<int> visibleColumns;
        int totalWidth = 0;
        for (int column = 0; column < model()->columnCount(); ++column) {
            if (isColumnHidden(column)) {
                continue;
            }
            visibleColumns.push_back(column);
            totalWidth += std::max(minWidthForColumn(column), columnWidth(column));
        }
        if (visibleColumns.isEmpty()) {
            return;
        }

        const int targetWidth = viewport()->width();
        if (targetWidth <= 0 || totalWidth == targetWidth) {
            return;
        }

        const QSignalBlocker blocker(horizontalHeader());
        m_fittingColumns = true;

        QVector<int> adjustableColumns;
        int fixedWidth = 0;
        int adjustableWidth = 0;
        for (int column : visibleColumns) {
            const int width = std::max(minWidthForColumn(column), columnWidth(column));
            if (column == fixedColumn) {
                fixedWidth += width;
                continue;
            }
            adjustableColumns.push_back(column);
            adjustableWidth += width;
        }

        if (adjustableColumns.isEmpty()) {
            m_fittingColumns = false;
            return;
        }

        int sumAdjustableMin = 0;
        for (int col : adjustableColumns) {
            sumAdjustableMin += minWidthForColumn(col);
        }

        // If a fixed (dragged) column would push the adjustable columns below their
        // minimums, clamp it so the table never overflows the viewport width.
        if (fixedColumn >= 0) {
            const int maxFixed = targetWidth - sumAdjustableMin;
            if (fixedWidth > maxFixed) {
                fixedWidth = std::max(minWidthForColumn(fixedColumn), maxFixed);
                setColumnWidth(fixedColumn, fixedWidth);
            }
        }

        const int availableWidth = std::max(sumAdjustableMin, targetWidth - fixedWidth);
        int remainingWidth = availableWidth;
        for (int index = 0; index < adjustableColumns.size(); ++index) {
            const int column = adjustableColumns.at(index);
            const int minW = minWidthForColumn(column);
            const int width = index == adjustableColumns.size() - 1
                ? std::max(minW, remainingWidth)
                : std::max(minW, (columnWidth(column) * availableWidth) / std::max(1, adjustableWidth));
            setColumnWidth(column, width);
            remainingWidth -= width;
        }

        m_fittingColumns = false;
    }

    std::function<void(const QVector<int> &rows, int destinationRow)> rowsMoveRequested;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTableView::resizeEvent(event);
        fitVisibleColumnsToViewport();
    }

    bool isQueueDrag(const QMimeData *mime) const
    {
        return mime != nullptr && mime->hasFormat(QString::fromLatin1(queueRowsMimeType));
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (isQueueDrag(event->mimeData())) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            QTableView::dragEnterEvent(event);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (isQueueDrag(event->mimeData())) {
            // Accept the move across the whole viewport (including above the first
            // row) so items can be dropped upward, not only downward.
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            QTableView::dragMoveEvent(event);
        }
        setDropIndicatorRow(rowForDropPosition(event->position().toPoint()));
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        QTableView::dragLeaveEvent(event);
        setDropIndicatorRow(-1);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (isQueueDrag(event->mimeData())) {
            QVector<int> rows;
            QByteArray payload = event->mimeData()->data(QString::fromLatin1(queueRowsMimeType));
            QDataStream stream(&payload, QIODevice::ReadOnly);
            stream >> rows;
            if (rowsMoveRequested) {
                rowsMoveRequested(rows, rowForDropPosition(event->position().toPoint()));
            }
            event->acceptProposedAction();
        } else {
            QTableView::dropEvent(event);
        }
        setDropIndicatorRow(-1);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTableView::paintEvent(event);
        if (model() == nullptr) {
            return;
        }

        QPainter painter(viewport());

        // Currently-playing indicator: a left accent bar, drawn at the row level
        // so it is independent of which columns are shown (paired with the row
        // tint from the item delegate).
        if (m_currentPlayingRow >= 0 && m_currentPlayingRow < model()->rowCount()) {
            const int y = rowViewportPosition(m_currentPlayingRow);
            const int h = rowHeight(m_currentPlayingRow);
            if (h > 0 && y + h > 0 && y < viewport()->height()) {
                painter.fillRect(QRect(0, y, 3, h), palette().color(QPalette::Highlight));
            }
        }

        if (m_dropIndicatorRow >= 0) {
            const int y = yForDropRow(m_dropIndicatorRow);
            QColor color = palette().color(QPalette::Highlight);
            color.setAlpha(210);
            QPen pen(color, 2);
            painter.setPen(pen);
            painter.drawLine(QLine(0, y, viewport()->width(), y));
        }
    }

private:
    static int preferredWidthForColumn(int column)
    {
        for (const ColumnSpec &spec : columns) {
            if (spec.index == column) {
                return spec.preferredWidth;
            }
        }
        return 60;
    }

    static int minWidthForColumn(int column)
    {
        for (const ColumnSpec &spec : columns) {
            if (spec.index == column) {
                return spec.minWidth;
            }
        }
        return 24;
    }

    int rowForDropPosition(const QPoint &pos) const
    {
        const QModelIndex index = indexAt(pos);
        if (!index.isValid()) {
            return model() == nullptr ? -1 : model()->rowCount();
        }
        const QRect rect = visualRect(index);
        return pos.y() < rect.center().y() ? index.row() : index.row() + 1;
    }

    int yForDropRow(int row) const
    {
        if (model() == nullptr || model()->rowCount() == 0) {
            return 0;
        }
        // Use row geometry rather than visualRect(column 0) so the indicator
        // position is independent of whether the "#" column is hidden.
        const int lastRow = model()->rowCount() - 1;
        if (row > lastRow) {
            return rowViewportPosition(lastRow) + rowHeight(lastRow);
        }
        return rowViewportPosition(row);
    }

    void setDropIndicatorRow(int row)
    {
        if (m_dropIndicatorRow == row) {
            return;
        }
        m_dropIndicatorRow = row;
        viewport()->update();
    }

    int m_dropIndicatorRow = -1;
    int m_currentPlayingRow = -1;
    bool m_fittingColumns = false;
};

// Delegate for the queue table: replicates DenseTableDelegate painting and
// overlays a small dimmed play-next ordinal at the right edge of the title cell
// when the title-accent display mode is enabled.
class QueueItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit QueueItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void setHoveredRow(int row)
    {
        m_hoveredRow = row;
    }

    void setShowTitleAccent(bool show)
    {
        m_showTitleAccent = show;
    }

    void setCurrentRow(int row)
    {
        m_currentRow = row;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = (m_hoveredRow == index.row()) || (opt.state & QStyle::State_MouseOver);
        const bool playing = index.row() == m_currentRow;

        if (selected) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::Highlight));
        } else if (hovered) {
            QColor hover = opt.palette.color(QPalette::Highlight);
            hover.setAlpha(34);
            painter->fillRect(opt.rect, hover);
        } else if (playing) {
            // Distinct, column-independent tint for the currently playing row
            // (every visible cell, regardless of which columns are shown).
            QColor tint = opt.palette.color(QPalette::Highlight);
            tint.setAlpha(48);
            painter->fillRect(opt.rect, tint);
        } else if (index.row() % 2 == 1) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
        }

        if (selected) {
            opt.palette.setColor(QPalette::Text, opt.palette.color(QPalette::HighlightedText));
        } else {
            opt.state &= ~QStyle::State_MouseOver;
            opt.state &= ~QStyle::State_Selected;
        }
        opt.features &= ~QStyleOptionViewItem::Alternate;
        opt.backgroundBrush = Qt::NoBrush;
        opt.displayAlignment = opt.displayAlignment | Qt::AlignVCenter;
        QStyledItemDelegate::paint(painter, opt, index);

        if (m_showTitleAccent && index.column() == 1) {
            const int ordinal = index.data(PlayNextOrdinalRole).toInt();
            if (ordinal > 0) {
                painter->save();
                QColor color = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Highlight);
                color.setAlpha(selected ? 160 : 130);
                painter->setPen(color);
                QFont f = painter->font();
                f.setPointSizeF(f.pointSizeF() * 0.8);
                painter->setFont(f);
                painter->drawText(option.rect.adjusted(0, 0, -4, 0),
                                  Qt::AlignRight | Qt::AlignVCenter,
                                  QString::number(ordinal));
                painter->restore();
            }
        }
    }

private:
    int m_hoveredRow = -1;
    int m_currentRow = -1;
    bool m_showTitleAccent = false;
};

QString displayYear(const Track &track)
{
    for (const QString &candidate : {track.originalDate, track.date}) {
        const QString trimmed = candidate.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed.left(4);
        }
    }
    return {};
}

class QueueTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit QueueTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_tracks.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 9;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        for (const ColumnSpec &spec : columns) {
            if (spec.index == section) {
                return QString::fromLatin1(spec.label);
            }
        }
        return {};
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_tracks.size() || role != Qt::DisplayRole) {
            if (index.isValid() && index.row() >= 0 && index.row() < m_tracks.size()) {
                const Track &track = m_tracks.at(index.row());
                if (role == TrackRole) {
                    return QVariant::fromValue(track);
                }
                if (role == PlayNextOrdinalRole) {
                    return playNextOrdinalForRow(index.row());
                }
                if (index.column() == 2 && role == Qt::UserRole) {
                    return track.effectiveRating0To100;
                }
                if (index.column() == 2 && role == HoverRatingRole) {
                    return m_hoverRatings.value(index.row(), StarRating::unset);
                }
            }
            return {};
        }

        const Track &track = m_tracks.at(index.row());
        switch (index.column()) {
        case 0: {
            if (m_showPlayNextBadge) {
                const int ordinal = playNextOrdinalForRow(index.row());
                if (ordinal > 0) {
                    return QStringLiteral("▸%1").arg(ordinal);
                }
            }
            return QString::number(index.row() + 1);
        }
        case 1:
            return track.title;
        case 2:
            return {};
        case 3:
            return ratingText(track.effectiveRating0To100);
        case 4:
            return track.artistName;
        case 5:
            return track.albumTitle;
        case 6:
            return formatDuration(track.durationMs);
        case 7:
            return displayYear(track);
        case 8:
            return trackNumberText(track);
        default:
            return {};
        }
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!index.isValid() || index.column() != 2 || role != HoverRatingRole || index.row() >= m_hoverRatings.size()) {
            return false;
        }
        m_hoverRatings[index.row()] = value.toInt();
        emit dataChanged(index, index, {HoverRatingRole});
        return true;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (index.isValid()) {
            // Rows are draggable but NOT drop targets: drops only land *between*
            // rows (above/below), never "onto" a row, so there is no third
            // ambiguous "over" snap state.
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        }
        return Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;
    }

    Qt::DropActions supportedDropActions() const override
    {
        return Qt::MoveAction;
    }

    QStringList mimeTypes() const override
    {
        return {QString::fromLatin1(queueRowsMimeType)};
    }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        auto *mime = new QMimeData;
        QVector<int> rows;
        rows.reserve(indexes.size());
        for (const QModelIndex &index : indexes) {
            if (index.isValid() && !rows.contains(index.row())) {
                rows.push_back(index.row());
            }
        }
        std::sort(rows.begin(), rows.end());

        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream << rows;
        mime->setData(QString::fromLatin1(queueRowsMimeType), payload);
        return mime;
    }

    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int, const QModelIndex &parent) override
    {
        if (action != Qt::MoveAction || data == nullptr || !data->hasFormat(QString::fromLatin1(queueRowsMimeType))) {
            return false;
        }

        QVector<int> rows;
        QByteArray payload = data->data(QString::fromLatin1(queueRowsMimeType));
        QDataStream stream(&payload, QIODevice::ReadOnly);
        stream >> rows;
        int destinationRow = row;
        if (destinationRow < 0 && parent.isValid()) {
            destinationRow = parent.row();
        }
        if (destinationRow < 0) {
            destinationRow = rowCount();
        }
        emit rowsMoveRequested(rows, destinationRow);
        return true;
    }

    void setTracks(const QVector<Track> &tracks)
    {
        beginResetModel();
        m_tracks = tracks;
        m_hoverRatings.fill(StarRating::unset, m_tracks.size());
        endResetModel();
    }

    // Sets the half-open range [begin, end) of rows that are "play next" priority
    // tracks. Rows outside the range (or when begin >= end) have ordinal 0.
    void setPlayNextRange(int begin, int end)
    {
        if (m_playNextBegin == begin && m_playNextEnd == end) {
            return;
        }
        m_playNextBegin = begin;
        m_playNextEnd = end;
        if (rowCount() > 0) {
            emit dataChanged(index(0, 0), index(rowCount() - 1, 1));
        }
    }

    void setShowPlayNextBadge(bool show)
    {
        if (m_showPlayNextBadge == show) {
            return;
        }
        m_showPlayNextBadge = show;
        if (rowCount() > 0) {
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DisplayRole});
        }
    }

signals:
    void rowsMoveRequested(QVector<int> rows, int destinationRow) const;

private:
    int playNextOrdinalForRow(int row) const
    {
        if (m_playNextBegin < 0 || m_playNextEnd <= m_playNextBegin
            || row < m_playNextBegin || row >= m_playNextEnd) {
            return 0;
        }
        return row - m_playNextBegin + 1;
    }

    QVector<Track> m_tracks;
    QVector<int> m_hoverRatings;
    int m_playNextBegin = -1;
    int m_playNextEnd = -1;
    bool m_showPlayNextBadge = true;
};

struct TrackInfoField {
    QString key;
    QString label;
    bool visible = true;
    int opacity = 50;
    int sizeDelta = 0;
};

enum class TrackInfoOverflowMode {
    Scroll,
    Truncate,
};

QVector<TrackInfoField> defaultTrackInfoFields()
{
    return {
        {QStringLiteral("title"), QStringLiteral("Title"), true, 90, 1},
        {QStringLiteral("artist"), QStringLiteral("Artist"), true, 50, 0},
        {QStringLiteral("album"), QStringLiteral("Album"), true, 50, 0},
        {QStringLiteral("date"), QStringLiteral("Date"), true, 40, 0},
        {QStringLiteral("metadata"), QStringLiteral("Metadata"), true, 40, 0},
        {QStringLiteral("file"), QStringLiteral("File full path"), true, 35, -1},
    };
}

class TrackInfoLabel final : public QLabel {
public:
    explicit TrackInfoLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setTextInteractionFlags(Qt::NoTextInteraction);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_scrollTimer.setInterval(30);
        connect(&m_scrollTimer, &QTimer::timeout, this, [this]() {
            advanceScroll();
        });
    }

    void setFullText(const QString &text)
    {
        m_fullText = text;
        setToolTip(text);
        resetScrollState();
        update();
    }

    QString fullText() const
    {
        return m_fullText;
    }

    void setClickable(bool clickable)
    {
        m_clickable = clickable;
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    void setOverflowMode(TrackInfoOverflowMode mode)
    {
        if (m_overflowMode == mode) {
            return;
        }
        m_overflowMode = mode;
        resetScrollState();
        update();
    }

    std::function<void()> clicked;

protected:
    void enterEvent(QEnterEvent *event) override
    {
        m_hovered = true;
        QLabel::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        m_hovered = false;
        QLabel::leaveEvent(event);
        update();
    }

    QSize sizeHint() const override
    {
        return {QLabel::sizeHint().width(), fontMetrics().height() + 2};
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        resetScrollState();
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange) {
            resetScrollState();
        } else if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange) {
            update();
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_clickable && event->button() == Qt::LeftButton && textBounds().contains(event->pos()) && clicked) {
            clicked();
            return;
        }
        QLabel::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QColor color = palette().color(QPalette::WindowText);
        const int opacity = property("muzaitenOpacity").toInt();
        if (opacity > 0) {
            color.setAlphaF(static_cast<float>(std::clamp(opacity / 100.0, 0.1, 1.0)));
        }
        painter.setPen(color);

        QFont styled = clickableFont();
        painter.setFont(styled);

        const QRect area = contentsRect();
        if (area.width() <= 0 || area.height() <= 0 || m_fullText.isEmpty()) {
            return;
        }

        const QFontMetrics fm(styled);
        const int textWidth = fm.horizontalAdvance(m_fullText);
        const int baseline = area.top() + ((area.height() - fm.height()) / 2) + fm.ascent();
        const bool overflowing = textWidth > area.width();
        if (!overflowing || m_overflowMode == TrackInfoOverflowMode::Truncate) {
            const QString text = overflowing ? compactDisplayText(fm, area.width()) : m_fullText;
            painter.drawText(area.left(), baseline, text);
            return;
        }

        const int gap = std::max(24, fm.horizontalAdvance(QStringLiteral("   ")));
        painter.setClipRect(area);
        painter.drawText(area.left() - m_scrollOffset, baseline, m_fullText);
        painter.drawText(area.left() - m_scrollOffset + textWidth + gap, baseline, m_fullText);
    }

private:
    QRect textBounds() const
    {
        const QRect area = contentsRect();
        const QFont styled = clickableFont();
        const QFontMetrics fm(styled);
        const int visibleWidth = (m_overflowMode == TrackInfoOverflowMode::Scroll)
            ? std::min(fm.horizontalAdvance(m_fullText), area.width())
            : fm.horizontalAdvance(compactDisplayText(fm, area.width()));
        const int textHeight = fm.height();
        return QRect(area.left(),
                     area.top() + ((area.height() - textHeight) / 2),
                     std::max(0, visibleWidth),
                     textHeight);
    }

    QFont clickableFont() const
    {
        QFont styled = font();
        styled.setUnderline(m_clickable && m_hovered);
        return styled;
    }

    QString compactDisplayText(const QFontMetrics &fm, int width) const
    {
        if (width <= 0) {
            return {};
        }
        if (!m_fullText.contains(QLatin1Char('/')) && !m_fullText.contains(QLatin1Char('\\'))) {
            return fm.elidedText(m_fullText, Qt::ElideMiddle, width);
        }

        const QString cleaned = QDir::cleanPath(m_fullText);
        const QFileInfo info(cleaned);
        const QString fileName = info.fileName();
        const QStringList parts = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QStringList candidates;
        if (!parts.isEmpty() && !fileName.isEmpty()) {
            const QString root = cleaned.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString();
            if (parts.size() >= 2) {
                candidates.push_back(root + parts.at(0) + QStringLiteral("/.../") + fileName);
            }
            if (parts.size() >= 3) {
                candidates.push_back(root + parts.at(0) + QLatin1Char('/') + parts.at(1) + QStringLiteral("/.../") + fileName);
            }
            candidates.push_back(QStringLiteral(".../") + fileName);
        }
        candidates.push_back(cleaned);

        for (const QString &candidate : candidates) {
            if (fm.horizontalAdvance(candidate) <= width) {
                return candidate;
            }
        }
        return fm.elidedText(candidates.isEmpty() ? cleaned : candidates.constFirst(), Qt::ElideMiddle, width);
    }

    void resetScrollState()
    {
        m_scrollOffset = 0;
        m_pauseTicksRemaining = 100;
        updateScrollTimer();
        update();
    }

    void updateScrollTimer()
    {
        const bool needsScroll = m_overflowMode == TrackInfoOverflowMode::Scroll
            && fontMetrics().horizontalAdvance(m_fullText) > contentsRect().width()
            && !m_fullText.isEmpty();
        if (needsScroll) {
            if (!m_scrollTimer.isActive()) {
                m_scrollTimer.start();
            }
        } else {
            m_scrollTimer.stop();
        }
    }

    void advanceScroll()
    {
        const QFontMetrics fm(clickableFont());
        const int textWidth = fm.horizontalAdvance(m_fullText);
        const int gap = std::max(24, fm.horizontalAdvance(QStringLiteral("   ")));
        if (textWidth <= contentsRect().width()) {
            m_scrollTimer.stop();
            return;
        }
        if (m_pauseTicksRemaining > 0) {
            --m_pauseTicksRemaining;
            return;
        }
        ++m_scrollOffset;
        if (m_scrollOffset >= textWidth + gap) {
            m_scrollOffset = 0;
            m_pauseTicksRemaining = 100;
        }
        update();
    }

    QString m_fullText;
    bool m_clickable = false;
    bool m_hovered = false;
    TrackInfoOverflowMode m_overflowMode = TrackInfoOverflowMode::Scroll;
    QTimer m_scrollTimer;
    int m_scrollOffset = 0;
    int m_pauseTicksRemaining = 0;
};

class AlbumArtLabel final : public QLabel {
public:
    explicit AlbumArtLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setAlignment(Qt::AlignCenter);
        setFrameShape(QFrame::StyledPanel);
        setScaledContents(false);
    }

    void setSourcePath(const QString &path)
    {
        m_sourcePath = path;
        m_sourceImage = QImage();
        updateScaledPixmap();
    }

    void setSourceImage(const QImage &image)
    {
        m_sourceImage = image;
        m_sourcePath.clear();
        updateScaledPixmap();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateScaledPixmap();
    }

private:
    void updateScaledPixmap()
    {
        const QSize target = contentsRect().size();
        if (target.width() <= 0 || target.height() <= 0) {
            setPixmap({});
            return;
        }

        // Render at the device pixel ratio so covers stay crisp on HiDPI screens:
        // scale to the physical pixel size and tag the pixmap's DPR, otherwise the
        // image is produced at logical size and upscaled by the compositor (blurry).
        const qreal dpr = devicePixelRatioF();
        const QSize pxTarget(qRound(target.width() * dpr), qRound(target.height() * dpr));

        auto applyRaster = [&](const QPixmap &source) {
            if (source.isNull()) {
                return false;
            }
            QPixmap scaled = source.scaled(pxTarget, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            scaled.setDevicePixelRatio(dpr);
            setPixmap(scaled);
            return true;
        };

        if (!m_sourceImage.isNull()) {
            applyRaster(QPixmap::fromImage(m_sourceImage));
            return;
        }

        if (m_sourcePath.isEmpty()) {
            setPixmap({});
            return;
        }

        if (m_sourcePath.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
            QIcon icon(m_sourcePath);
            if (icon.isNull()) {
                return;
            }
            // The (size, dpr) overload bakes the device pixel ratio into the pixmap.
            setPixmap(icon.pixmap(target, dpr));
        } else {
            applyRaster(QPixmap(m_sourcePath));
        }
    }

    QString m_sourcePath;
    QImage m_sourceImage;
};

QString formatSize(qint64 bytes)
{
    if (bytes <= 0) {
        return {};
    }
    double value = static_cast<double>(bytes);
    const QStringList units = {QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")};
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, unit == 0 ? 'f' : 'f', unit == 0 ? 0 : 1).arg(units.at(unit));
}

QString ratingText(int rating0To100)
{
    if (rating0To100 < 0) {
        return {};
    }
    return QStringLiteral("%1 %2").arg(rating0To100 / 20.0, 0, 'f', 1).arg(QChar(0x2605));
}

TrackInfoLabel *valueLabel(QWidget *parent)
{
    auto *label = new TrackInfoLabel(parent);
    return label;
}

QString formatDuration(qint64 durationMs)
{
    if (durationMs <= 0) {
        return {};
    }

    const qint64 totalSeconds = durationMs / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString trackNumberText(const Track &track)
{
    if (track.trackNumber > 0) {
        return track.discNumber > 1
            ? QStringLiteral("%1.%2").arg(track.discNumber).arg(track.trackNumber, 2, 10, QLatin1Char('0'))
            : QString::number(track.trackNumber);
    }

    const QString base = QFileInfo(track.filename.isEmpty() ? track.path : track.filename).completeBaseName();
    int end = 0;
    while (end < base.size() && base.at(end).isDigit()) {
        ++end;
    }
    if (end > 0) {
        return base.left(end);
    }
    return {};
}

QString displayDate(const Track &track)
{
    if (!track.originalDate.isEmpty()) {
        return track.originalDate;
    }
    return track.date;
}

QString metadataText(const Track &track, const QString &pattern)
{
    QStringList parts;
    const QString suffix = QFileInfo(track.path).suffix().toUpper();
    if (!suffix.isEmpty()) {
        parts.push_back(suffix);
    }
    const QString duration = formatDuration(track.durationMs);
    if (!duration.isEmpty()) {
        parts.push_back(duration);
    }
    const QString size = formatSize(track.fileSize);
    if (!size.isEmpty()) {
        parts.push_back(size);
    }
    if (pattern.trimmed().isEmpty()) {
        return parts.join(QStringLiteral(", "));
    }
    QString text = pattern;
    text.replace(QStringLiteral("{format}"), suffix);
    text.replace(QStringLiteral("{duration}"), duration);
    text.replace(QStringLiteral("{size}"), size);
    return text.simplified();
}

} // namespace

RightSidebar::RightSidebar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter, 1);

    auto *queueModel = new QueueTableModel(this);
    auto *queueView = new QueueTableView(m_splitter);
    m_queueTable = queueView;
    m_queueTable->setModel(queueModel);
    m_queueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queueTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queueTable->setDragEnabled(true);
    m_queueTable->setAcceptDrops(true);
    m_queueTable->setDropIndicatorShown(false); // we draw our own above/below line
    m_queueTable->setDragDropMode(QAbstractItemView::InternalMove);
    m_queueTable->setDefaultDropAction(Qt::MoveAction);
    m_queueTable->setDragDropOverwriteMode(false);
    auto *queueItemDelegate = new QueueItemDelegate(this);
    m_queueTable->setItemDelegate(queueItemDelegate);
    auto *queueRatingDelegate = new StarRatingDelegate(this);
    m_queueTable->setItemDelegateForColumn(2, queueRatingDelegate);
    m_queueTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_queueTable->setShowGrid(false);
    m_queueTable->setWordWrap(false);
    m_queueTable->setMouseTracking(true);
    m_queueTable->viewport()->setMouseTracking(true);
    m_queueTable->verticalHeader()->setVisible(false);
    m_queueTable->verticalHeader()->setDefaultSectionSize(18);
    m_queueTable->verticalHeader()->setMinimumSectionSize(18);
    m_queueTable->horizontalHeader()->setFixedHeight(20);
    m_queueTable->horizontalHeader()->setMinimumSectionSize(8);
    m_queueTable->horizontalHeader()->setSectionsMovable(true);
    m_queueTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_queueTable->horizontalHeader()->setStretchLastSection(false);
    m_queueTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_queueTable->setAlternatingRowColors(true);
    m_queueTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_queueTable->setStyleSheet(QStringLiteral("QTableView::item { padding: 0 3px; }"));
    m_queueTable->viewport()->installEventFilter(this);
    for (const int column : {2, 4, 5, 6, 7, 8}) {
        m_queueTable->setColumnHidden(column, true);
    }
    for (const ColumnSpec &spec : columns) {
        m_queueTable->setColumnWidth(spec.index, spec.preferredWidth);
    }
    static_cast<QueueTableView *>(m_queueTable)->fitVisibleColumnsToViewport();
    m_splitter->addWidget(m_queueTable);

    connect(m_queueTable, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.isValid()) {
            emit queueTrackActivated(index.row());
        }
    });
    connect(m_queueTable->horizontalHeader(), &QHeaderView::customContextMenuRequested, this, &RightSidebar::showHeaderMenu);
    connect(m_queueTable->horizontalHeader(), &QHeaderView::sectionMoved, this, [this]() {
        emit viewSettingsChanged();
    });
    // User column resizing trades width with the adjacent column only (no global
    // redistribute); the viewport-fill fit still runs on pane resize/show-hide.
    auto *queueColumnResizer = NeighborColumnResizer::install(
        m_queueTable->horizontalHeader(), [](int column) {
            for (const ColumnSpec &spec : columns) {
                if (spec.index == column) {
                    return spec.minWidth;
                }
            }
            return 24;
        });
    connect(queueColumnResizer, &NeighborColumnResizer::columnResized, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(m_queueTable->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
        static_cast<QueueTableView *>(m_queueTable)->fitVisibleColumnsToViewport();
    });
    connect(m_queueTable, &QWidget::customContextMenuRequested, this, &RightSidebar::showQueueMenu);
    queueView->rowsMoveRequested = [this](const QVector<int> &rows, int destinationRow) {
        emit queueRowsMoveRequested(rows, destinationRow);
    };
    connect(queueRatingDelegate, &StarRatingDelegate::ratingEdited, this, [this](const QModelIndex &index, int rating) {
        const Track track = index.data(TrackRole).value<Track>();
        if (!track.path.isEmpty()) {
            emit queueTrackRatingChanged(track, rating);
        }
    });
    OverlayScrollBar::install(m_queueTable);

    m_trackInfoPane = new QFrame(m_splitter);
    auto *infoLayout = new QVBoxLayout(m_trackInfoPane);
    infoLayout->setContentsMargins(6, 6, 6, 6);
    infoLayout->setSpacing(1);
    m_noTrackLabel = new QLabel(QStringLiteral("No track playing"), m_trackInfoPane);
    m_noTrackLabel->setAlignment(Qt::AlignCenter);
    infoLayout->addWidget(m_noTrackLabel, 1);
    m_trackInfoTitle = valueLabel(m_trackInfoPane);
    m_trackInfoArtist = valueLabel(m_trackInfoPane);
    m_trackInfoAlbum = valueLabel(m_trackInfoPane);
    m_trackInfoYear = valueLabel(m_trackInfoPane);
    m_trackInfoProperties = valueLabel(m_trackInfoPane);
    m_trackInfoFile = valueLabel(m_trackInfoPane);
    for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
        label->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(label, &QWidget::customContextMenuRequested, this, &RightSidebar::showTrackInfoLabelMenu);
        infoLayout->addWidget(label);
    }
    static_cast<TrackInfoLabel *>(m_trackInfoArtist)->setClickable(true);
    static_cast<TrackInfoLabel *>(m_trackInfoAlbum)->setClickable(true);
    static_cast<TrackInfoLabel *>(m_trackInfoFile)->setClickable(true);
    static_cast<TrackInfoLabel *>(m_trackInfoArtist)->clicked = [this]() {
        emit artistRequested(m_currentTrack.albumArtistName.isEmpty() ? m_currentTrack.artistName : m_currentTrack.albumArtistName);
    };
    static_cast<TrackInfoLabel *>(m_trackInfoAlbum)->clicked = [this]() {
        emit albumRequested(m_currentTrack.albumArtistName.isEmpty() ? m_currentTrack.artistName : m_currentTrack.albumArtistName, m_currentTrack.albumTitle);
    };
    static_cast<TrackInfoLabel *>(m_trackInfoFile)->clicked = [this]() {
        emit findFileRequested(m_currentTrack);
    };
    restyleTrackInfoLabels();
    m_trackInfoPane->setMinimumHeight(96);
    m_splitter->addWidget(m_trackInfoPane);

    m_albumArt = new AlbumArtLabel(m_splitter);
    m_albumArt->setMinimumSize(180, 180);
    m_albumArt->setText(QStringLiteral("Album art"));
    m_splitter->addWidget(m_albumArt);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({420, 150, 240});
    setTrackInfo({});

    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        emit viewSettingsChanged();
    });
}

void RightSidebar::setQueue(const QVector<Track> &tracks)
{
    m_tracks = tracks;
    if (auto *queueModel = qobject_cast<QueueTableModel *>(m_queueTable->model())) {
        queueModel->setTracks(tracks);
    }
    // Defer until Qt finalizes the post-reset layout (scrollbar visibility may change).
    QTimer::singleShot(0, this, [this]() {
        static_cast<QueueTableView *>(m_queueTable)->fitVisibleColumnsToViewport();
    });
}

void RightSidebar::setPlayNextRange(int begin, int end)
{
    m_playNextBegin = begin;
    m_playNextEnd = end;
    if (auto *queueModel = qobject_cast<QueueTableModel *>(m_queueTable->model())) {
        queueModel->setPlayNextRange(begin, end);
    }
}

void RightSidebar::setCurrentIndex(int index, bool reveal)
{
    // The "currently playing" row is shown with its own indicator and is kept
    // independent of the user's selection, so reordering/adding/removing never
    // hijacks the selection or scrolls the view. Only explicit playback changes
    // (reveal == true) scroll the playing row into view.
    const int rowCount = m_queueTable->model()->rowCount();
    const int current = (index >= 0 && index < rowCount) ? index : -1;
    m_currentIndex = current;

    static_cast<QueueTableView *>(m_queueTable)->setCurrentPlayingRow(current);
    if (auto *delegate = qobject_cast<QueueItemDelegate *>(m_queueTable->itemDelegate())) {
        delegate->setCurrentRow(current);
    }
    m_queueTable->viewport()->update();

    if (reveal && current >= 0) {
        m_queueTable->scrollTo(m_queueTable->model()->index(current, 0), QAbstractItemView::PositionAtCenter);
    }
}

void RightSidebar::setAlbumArt(const QString &imagePath)
{
    const bool valid = !imagePath.isEmpty() && QFileInfo::exists(imagePath);
    const QString effectivePath = valid ? imagePath : AlbumArtFallback::resourcePath(palette());
    m_usingArtFallback = !valid;

    if (effectivePath.isEmpty()) {
        m_albumArt->setPixmap({});
        m_albumArt->setText(QStringLiteral("Album art"));
        return;
    }

    m_albumArt->setText({});
    static_cast<AlbumArtLabel *>(m_albumArt)->setSourcePath(effectivePath);
}

void RightSidebar::setAlbumArt(const QImage &image)
{
    if (image.isNull()) {
        setAlbumArt(QString());
        return;
    }
    m_usingArtFallback = false;
    m_albumArt->setText({});
    static_cast<AlbumArtLabel *>(m_albumArt)->setSourceImage(image);
}

void RightSidebar::setTrackInfo(const Track &track)
{
    m_currentTrack = track;
    updateTrackInfoLabels();
}

void RightSidebar::updateTrackInfoLabels()
{
    const bool hasTrack = !m_currentTrack.path.isEmpty();
    m_noTrackLabel->setVisible(!hasTrack);
    for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
        label->setVisible(hasTrack && !label->property("muzaitenHidden").toBool());
    }
    if (!hasTrack) {
        return;
    }

    static_cast<TrackInfoLabel *>(m_trackInfoTitle)->setFullText(m_currentTrack.title.isEmpty() ? m_currentTrack.filename : m_currentTrack.title);
    static_cast<TrackInfoLabel *>(m_trackInfoArtist)->setFullText(m_currentTrack.artistName);
    static_cast<TrackInfoLabel *>(m_trackInfoAlbum)->setFullText(m_currentTrack.albumTitle);
    static_cast<TrackInfoLabel *>(m_trackInfoYear)->setFullText(displayDate(m_currentTrack));
    static_cast<TrackInfoLabel *>(m_trackInfoProperties)->setFullText(metadataText(m_currentTrack, m_trackInfoMetadataPattern));
    static_cast<TrackInfoLabel *>(m_trackInfoFile)->setFullText(m_currentTrack.path);
    m_trackInfoFile->setToolTip(m_currentTrack.path);
}

void RightSidebar::setTrackInfoVisible(bool visible)
{
    m_trackInfoPane->setVisible(visible);
}

void RightSidebar::configureTrackInfoPanel(QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Track Information"));
    dialog.resize(640, 420);
    auto *layout = new QVBoxLayout(&dialog);

    auto *table = new QTableWidget(0, 4, &dialog);
    table->setHorizontalHeaderLabels({
        QStringLiteral("Show"),
        QStringLiteral("Field"),
        QStringLiteral("Opacity"),
        QStringLiteral("Size"),
    });
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    const QMap<QString, QString> labels = {
        {QStringLiteral("title"), QStringLiteral("<Title>")},
        {QStringLiteral("artist"), QStringLiteral("<Artist>")},
        {QStringLiteral("album"), QStringLiteral("<Album>")},
        {QStringLiteral("date"), QStringLiteral("<Date>")},
        {QStringLiteral("metadata"), QStringLiteral("<Metadata>")},
        {QStringLiteral("file"), QStringLiteral("<File full path>")},
    };

    for (const QJsonValue &value : trackInfoSettingsJson()) {
        const QJsonObject field = value.toObject();
        const int row = table->rowCount();
        table->insertRow(row);
        auto *show = new QCheckBox(table);
        show->setChecked(field.value(QStringLiteral("visible")).toBool(true));
        auto *showCell = new QWidget(table);
        auto *showLayout = new QHBoxLayout(showCell);
        showLayout->setContentsMargins(0, 0, 0, 0);
        showLayout->addWidget(show, 0, Qt::AlignCenter);
        table->setCellWidget(row, 0, showCell);
        auto *fieldItem = new QTableWidgetItem(labels.value(field.value(QStringLiteral("key")).toString()));
        fieldItem->setData(Qt::UserRole, field.value(QStringLiteral("key")).toString());
        table->setItem(row, 1, fieldItem);
        auto *opacity = new QSpinBox(table);
        opacity->setRange(10, 100);
        opacity->setSuffix(QStringLiteral("%"));
        opacity->setValue(field.value(QStringLiteral("opacity")).toInt(50));
        table->setCellWidget(row, 2, opacity);
        auto *size = new QSpinBox(table);
        size->setRange(-4, 6);
        size->setValue(field.value(QStringLiteral("sizeDelta")).toInt(0));
        table->setCellWidget(row, 3, size);
    }

    auto *buttonRow = new QHBoxLayout;
    auto *up = new QPushButton(QStringLiteral("Up"), &dialog);
    auto *down = new QPushButton(QStringLiteral("Down"), &dialog);
    buttonRow->addWidget(up);
    buttonRow->addWidget(down);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *metadataRow = new QHBoxLayout;
    metadataRow->addWidget(new QLabel(QStringLiteral("Metadata format"), &dialog));
    auto *metadataPattern = new QLineEdit(&dialog);
    metadataPattern->setPlaceholderText(QStringLiteral("{format}, {duration}, {size}"));
    metadataPattern->setText(m_trackInfoMetadataPattern);
    metadataRow->addWidget(metadataPattern, 1);
    layout->addLayout(metadataRow);

    auto *overflowRow = new QHBoxLayout;
    overflowRow->addWidget(new QLabel(QStringLiteral("Overflow"), &dialog));
    auto *overflowMode = new QComboBox(&dialog);
    overflowMode->addItem(QStringLiteral("Scroll"));
    overflowMode->addItem(QStringLiteral("Truncate"));
    overflowMode->setCurrentIndex(m_trackInfoTitle->property("muzaitenOverflowMode").toString() == QStringLiteral("truncate") ? 1 : 0);
    overflowRow->addWidget(overflowMode, 1);
    layout->addLayout(overflowRow);

    auto moveSelected = [table](int direction) {
        const int row = table->currentRow();
        const int target = row + direction;
        if (row < 0 || target < 0 || target >= table->rowCount()) {
            return;
        }
        for (int column = 0; column < table->columnCount(); ++column) {
            if (QWidget *first = table->cellWidget(row, column)) {
                table->removeCellWidget(row, column);
                QWidget *second = table->cellWidget(target, column);
                table->removeCellWidget(target, column);
                table->setCellWidget(row, column, second);
                table->setCellWidget(target, column, first);
            } else {
                QTableWidgetItem *firstItem = table->takeItem(row, column);
                QTableWidgetItem *secondItem = table->takeItem(target, column);
                table->setItem(row, column, secondItem);
                table->setItem(target, column, firstItem);
            }
        }
        table->selectRow(target);
    };
    connect(up, &QPushButton::clicked, &dialog, [moveSelected]() {
        moveSelected(-1);
    });
    connect(down, &QPushButton::clicked, &dialog, [moveSelected]() {
        moveSelected(1);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QJsonObject root;
    QJsonArray fields;
    for (int row = 0; row < table->rowCount(); ++row) {
        QJsonObject field;
        field.insert(QStringLiteral("key"), table->item(row, 1)->data(Qt::UserRole).toString());
        auto *showCell = table->cellWidget(row, 0);
        auto *show = showCell == nullptr ? nullptr : showCell->findChild<QCheckBox *>();
        auto *opacity = qobject_cast<QSpinBox *>(table->cellWidget(row, 2));
        auto *size = qobject_cast<QSpinBox *>(table->cellWidget(row, 3));
        field.insert(QStringLiteral("visible"), show == nullptr || show->isChecked());
        field.insert(QStringLiteral("opacity"), opacity == nullptr ? 50 : opacity->value());
        field.insert(QStringLiteral("sizeDelta"), size == nullptr ? 0 : size->value());
        fields.append(field);
    }
    root.insert(QStringLiteral("trackInfoFields"), fields);
    root.insert(QStringLiteral("trackInfoMetadata"), metadataPattern->text());
    root.insert(QStringLiteral("trackInfoOverflowMode"), overflowMode->currentIndex() == 1 ? QStringLiteral("truncate") : QStringLiteral("scroll"));
    applyTrackInfoSettingsJson(root);
    emit viewSettingsChanged();
}

QString RightSidebar::viewSettingsJson() const
{
    QJsonArray visibleColumns;
    for (const ColumnSpec &spec : columns) {
        if (!m_queueTable->isColumnHidden(spec.index)) {
            visibleColumns.append(QString::fromLatin1(spec.key));
        }
    }

    QJsonObject root;
    root.insert(QStringLiteral("visibleColumns"), visibleColumns);
    root.insert(QStringLiteral("headerHeight"), m_queueTable->horizontalHeader()->height());
    root.insert(QStringLiteral("rowHeight"), m_queueTable->verticalHeader()->defaultSectionSize());
    root.insert(QStringLiteral("headerState"), QString::fromLatin1(m_queueTable->horizontalHeader()->saveState().toBase64()));
    root.insert(QStringLiteral("showTrackInfo"), m_trackInfoPane->isVisible());
    root.insert(QStringLiteral("trackInfoFields"), trackInfoSettingsJson());
    root.insert(QStringLiteral("trackInfoMetadata"), m_trackInfoMetadataPattern);
    const QString overflowMode = m_trackInfoTitle->property("muzaitenOverflowMode").toString();
    root.insert(QStringLiteral("trackInfoOverflowMode"), overflowMode.isEmpty() ? QStringLiteral("scroll") : overflowMode);
    QJsonArray splitterSizes;
    for (int size : m_splitter->sizes()) {
        splitterSizes.append(size);
    }
    root.insert(QStringLiteral("splitter"), splitterSizes);
    root.insert(QStringLiteral("showPlayNextBadge"), m_showPlayNextBadge);
    root.insert(QStringLiteral("showPlayNextTitleAccent"), m_showPlayNextTitleAccent);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void RightSidebar::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray visible = root.value(QStringLiteral("visibleColumns")).toArray();
    if (!visible.isEmpty()) {
        QStringList visibleKeys;
        for (const QJsonValue &value : visible) {
            visibleKeys.push_back(value.toString());
        }
        for (const ColumnSpec &spec : columns) {
            m_queueTable->setColumnHidden(spec.index, !visibleKeys.contains(QString::fromLatin1(spec.key)));
        }
        static_cast<QueueTableView *>(m_queueTable)->fitVisibleColumnsToViewport();
    }

    setHeaderHeight(root.value(QStringLiteral("headerHeight")).toInt(20));
    m_queueTable->verticalHeader()->setDefaultSectionSize(std::clamp(root.value(QStringLiteral("rowHeight")).toInt(18), 18, 48));
    const QByteArray headerState = QByteArray::fromBase64(root.value(QStringLiteral("headerState")).toString().toLatin1());
    if (!headerState.isEmpty()) {
        m_queueTable->horizontalHeader()->restoreState(headerState);
        static_cast<QueueTableView *>(m_queueTable)->fitVisibleColumnsToViewport();
    }
    const QJsonArray splitter = root.value(QStringLiteral("splitter")).toArray();
    QList<int> sizes;
    for (const QJsonValue &value : splitter) {
        sizes.push_back(value.toInt());
    }
    if (sizes.size() == 2) {
        m_splitter->setSizes({sizes.value(0), 150, sizes.value(1)});
    } else if (sizes.size() == 3) {
        m_splitter->setSizes(sizes);
    }
    setTrackInfoVisible(root.value(QStringLiteral("showTrackInfo")).toBool(true));
    applyTrackInfoSettingsJson(root);

    const bool badge = root.value(QStringLiteral("showPlayNextBadge")).toBool(true);
    m_showPlayNextBadge = badge;
    if (auto *queueModel = qobject_cast<QueueTableModel *>(m_queueTable->model())) {
        queueModel->setShowPlayNextBadge(badge);
    }
    const bool accent = root.value(QStringLiteral("showPlayNextTitleAccent")).toBool(false);
    m_showPlayNextTitleAccent = accent;
    if (auto *d = qobject_cast<QueueItemDelegate *>(m_queueTable->itemDelegate())) {
        d->setShowTitleAccent(accent);
    }
}

void RightSidebar::applyTrackInfoSettingsJson(const QJsonObject &root)
{
    const QJsonArray fields = root.value(QStringLiteral("trackInfoFields")).toArray();
    if (fields.isEmpty()) {
        restyleTrackInfoLabels();
        updateTrackInfoLabels();
        return;
    }

    const QMap<QString, QWidget *> labels = {
        {QStringLiteral("title"), m_trackInfoTitle},
        {QStringLiteral("artist"), m_trackInfoArtist},
        {QStringLiteral("album"), m_trackInfoAlbum},
        {QStringLiteral("date"), m_trackInfoYear},
        {QStringLiteral("metadata"), m_trackInfoProperties},
        {QStringLiteral("file"), m_trackInfoFile},
    };
    auto *layout = qobject_cast<QVBoxLayout *>(m_trackInfoPane->layout());
    for (const QJsonValue &value : fields) {
        const QJsonObject field = value.toObject();
        QWidget *label = labels.value(field.value(QStringLiteral("key")).toString());
        if (label == nullptr) {
            continue;
        }
        label->setProperty("muzaitenHidden", !field.value(QStringLiteral("visible")).toBool(true));
        label->setProperty("muzaitenOpacity", field.value(QStringLiteral("opacity")).toInt(label->property("muzaitenOpacity").toInt()));
        label->setProperty("muzaitenSizeDelta", field.value(QStringLiteral("sizeDelta")).toInt(label->property("muzaitenSizeDelta").toInt()));
        if (layout != nullptr) {
            layout->removeWidget(label);
            layout->addWidget(label);
        }
    }
    m_trackInfoMetadataPattern = root.value(QStringLiteral("trackInfoMetadata")).toString();
    const QString overflow = root.value(QStringLiteral("trackInfoOverflowMode")).toString(QStringLiteral("scroll"));
    for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
        label->setProperty("muzaitenOverflowMode", overflow);
        static_cast<TrackInfoLabel *>(label)->setOverflowMode(overflow == QStringLiteral("truncate")
                                                                  ? TrackInfoOverflowMode::Truncate
                                                                  : TrackInfoOverflowMode::Scroll);
    }
    restyleTrackInfoLabels();
    updateTrackInfoLabels();
}

QJsonArray RightSidebar::trackInfoSettingsJson() const
{
    const QMap<QWidget *, QString> keys = {
        {m_trackInfoTitle, QStringLiteral("title")},
        {m_trackInfoArtist, QStringLiteral("artist")},
        {m_trackInfoAlbum, QStringLiteral("album")},
        {m_trackInfoYear, QStringLiteral("date")},
        {m_trackInfoProperties, QStringLiteral("metadata")},
        {m_trackInfoFile, QStringLiteral("file")},
    };
    QJsonArray fields;
    auto *layout = qobject_cast<QVBoxLayout *>(m_trackInfoPane->layout());
    for (int index = 0; layout != nullptr && index < layout->count(); ++index) {
        QWidget *label = layout->itemAt(index)->widget();
        const QString key = keys.value(label);
        if (key.isEmpty()) {
            continue;
        }
        QJsonObject field;
        field.insert(QStringLiteral("key"), key);
        field.insert(QStringLiteral("visible"), !label->property("muzaitenHidden").toBool());
        field.insert(QStringLiteral("opacity"), label->property("muzaitenOpacity").toInt());
        field.insert(QStringLiteral("sizeDelta"), label->property("muzaitenSizeDelta").toInt());
        fields.append(field);
    }
    return fields;
}

void RightSidebar::restyleTrackInfoLabels()
{
    const auto defaults = defaultTrackInfoFields();
    const QMap<QString, QWidget *> labels = {
        {QStringLiteral("title"), m_trackInfoTitle},
        {QStringLiteral("artist"), m_trackInfoArtist},
        {QStringLiteral("album"), m_trackInfoAlbum},
        {QStringLiteral("date"), m_trackInfoYear},
        {QStringLiteral("metadata"), m_trackInfoProperties},
        {QStringLiteral("file"), m_trackInfoFile},
    };
    QFont baseFont = font();
    for (const TrackInfoField &field : defaults) {
        QWidget *label = labels.value(field.key);
        if (label == nullptr) {
            continue;
        }
        if (!label->property("muzaitenOpacity").isValid()) {
            label->setProperty("muzaitenOpacity", field.opacity);
        }
        if (!label->property("muzaitenSizeDelta").isValid()) {
            label->setProperty("muzaitenSizeDelta", field.sizeDelta);
        }
        if (!label->property("muzaitenHidden").isValid()) {
            label->setProperty("muzaitenHidden", !field.visible);
        }
        QFont styled = baseFont;
        styled.setPointSize(std::max(6, baseFont.pointSize() + label->property("muzaitenSizeDelta").toInt()));
        label->setFont(styled);
    }
    QPalette emptyPalette = palette();
    QColor emptyColor = emptyPalette.color(QPalette::Text);
    emptyColor.setAlphaF(0.75);
    emptyPalette.setColor(QPalette::WindowText, emptyColor);
    emptyPalette.setColor(QPalette::Text, emptyColor);
    m_noTrackLabel->setPalette(emptyPalette);
}

QWidget *RightSidebar::trackInfoLabelFromSender() const
{
    return qobject_cast<QWidget *>(sender());
}

void RightSidebar::setHeaderHeight(int height)
{
    m_queueTable->horizontalHeader()->setFixedHeight(std::clamp(height, 18, 40));
}

bool RightSidebar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_queueTable->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            const int step = wheel->angleDelta().y() > 0 ? 2 : -2;
            const int rowHeight = std::clamp(m_queueTable->verticalHeader()->defaultSectionSize() + step, 18, 48);
            m_queueTable->verticalHeader()->setDefaultSectionSize(rowHeight);
            emit viewSettingsChanged();
            wheel->accept();
            return true;
        }
    }
    if (watched == m_queueTable->viewport() && event->type() == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QModelIndex index = m_queueTable->indexAt(mouse->pos());
        setQueueHoveredRow(index.isValid() ? index.row() : -1);
    } else if (watched == m_queueTable->viewport() && event->type() == QEvent::Leave) {
        setQueueHoveredRow(-1);
    }
    return QWidget::eventFilter(watched, event);
}

void RightSidebar::setQueueHoveredRow(int row)
{
    if (m_queueHoveredRow == row) {
        return;
    }

    const int previous = m_queueHoveredRow;
    m_queueHoveredRow = row;
    if (auto *d = qobject_cast<QueueItemDelegate *>(m_queueTable->itemDelegate())) {
        d->setHoveredRow(row);
    }
    if (auto *ratingDelegate = qobject_cast<StarRatingDelegate *>(m_queueTable->itemDelegateForColumn(2))) {
        ratingDelegate->setHoveredRow(row);
    }
    if (previous >= 0) {
        m_queueTable->model()->setData(m_queueTable->model()->index(previous, 2), StarRating::unset, HoverRatingRole);
        const QRect rect = m_queueTable->visualRect(m_queueTable->model()->index(previous, 0));
        m_queueTable->viewport()->update(QRect(0, rect.top(), m_queueTable->viewport()->width(), rect.height()));
    }
    if (row >= 0) {
        const QRect rect = m_queueTable->visualRect(m_queueTable->model()->index(row, 0));
        m_queueTable->viewport()->update(QRect(0, rect.top(), m_queueTable->viewport()->width(), rect.height()));
    }
}

void RightSidebar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        restyleTrackInfoLabels();
        updateTrackInfoLabels();
        m_trackInfoPane->update();
        m_queueTable->viewport()->update();
        m_queueTable->horizontalHeader()->viewport()->update();
    }
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) && m_usingArtFallback) {
        setAlbumArt(QString());
    }
}

void RightSidebar::showHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    for (const ColumnSpec &spec : columns) {
        QAction *action = menu.addAction(QString::fromLatin1(spec.label));
        action->setCheckable(true);
        action->setChecked(!m_queueTable->isColumnHidden(spec.index));
        connect(action, &QAction::toggled, this, [this, column = spec.index](bool checked) {
            m_queueTable->setColumnHidden(column, !checked);
            if (checked && m_queueTable->columnWidth(column) <= 24) {
                for (const ColumnSpec &candidate : columns) {
                    if (candidate.index == column) {
                        m_queueTable->setColumnWidth(column, candidate.preferredWidth);
                        break;
                    }
                }
            }
            static_cast<QueueTableView *>(m_queueTable)->fitVisibleColumnsToViewport();
            emit viewSettingsChanged();
        });
    }
    menu.addSeparator();

    QAction *badgeAction = menu.addAction(QStringLiteral("Show play-next badge in # column"));
    badgeAction->setCheckable(true);
    badgeAction->setChecked(m_showPlayNextBadge);
    connect(badgeAction, &QAction::toggled, this, [this](bool checked) {
        m_showPlayNextBadge = checked;
        if (auto *queueModel = qobject_cast<QueueTableModel *>(m_queueTable->model())) {
            queueModel->setShowPlayNextBadge(checked);
        }
        emit viewSettingsChanged();
    });

    QAction *accentAction = menu.addAction(QStringLiteral("Show play-next ordinal in title"));
    accentAction->setCheckable(true);
    accentAction->setChecked(m_showPlayNextTitleAccent);
    connect(accentAction, &QAction::toggled, this, [this](bool checked) {
        m_showPlayNextTitleAccent = checked;
        if (auto *d = qobject_cast<QueueItemDelegate *>(m_queueTable->itemDelegate())) {
            d->setShowTitleAccent(checked);
        }
        m_queueTable->viewport()->update();
        emit viewSettingsChanged();
    });

    menu.exec(m_queueTable->horizontalHeader()->mapToGlobal(pos));
}

void RightSidebar::showQueueMenu(const QPoint &pos)
{
    const int row = m_queueTable->rowAt(pos.y());
    if (row < 0 || row >= m_tracks.size()) {
        return;
    }
    if (!m_queueTable->selectionModel()->isRowSelected(row, QModelIndex())) {
        m_queueTable->selectRow(row);
    }

    const Track track = m_tracks.at(row);
    QMenu menu(this);
    QAction *play = menu.addAction(QStringLiteral("Play"));
    connect(play, &QAction::triggered, this, [this, row]() {
        emit queueTrackActivated(row);
    });
    menu.addSeparator();
    if (row == m_currentIndex) {
        QAction *findCurrent = menu.addAction(QStringLiteral("Find current track in library"));
        connect(findCurrent, &QAction::triggered, this, &RightSidebar::currentTrackLibraryRequested);
    }
    QAction *findFile = menu.addAction(QStringLiteral("Open containing directory"));
    connect(findFile, &QAction::triggered, this, [this, track]() {
        emit findFileRequested(track);
    });
    QAction *removeSelected = menu.addAction(QStringLiteral("Remove selected"));
    connect(removeSelected, &QAction::triggered, this, [this]() {
        QVector<int> rows;
        const QModelIndexList selected = m_queueTable->selectionModel()->selectedRows();
        rows.reserve(selected.size());
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        emit queueRowsRemoveRequested(rows);
    });
    QAction *clearPlayNext = menu.addAction(QStringLiteral("Clear play next priority"));
    clearPlayNext->setEnabled(m_playNextEnd > m_playNextBegin && m_playNextBegin >= 0);
    connect(clearPlayNext, &QAction::triggered, this, [this]() {
        emit clearPlayNextPriorityRequested();
    });
    QAction *clearQueue = menu.addAction(QStringLiteral("Clear queue"));
    connect(clearQueue, &QAction::triggered, this, [this]() {
        emit queueClearRequested();
    });
    menu.exec(m_queueTable->viewport()->mapToGlobal(pos));
}

void RightSidebar::showTrackInfoLabelMenu(const QPoint &pos)
{
    QWidget *label = trackInfoLabelFromSender();
    if (label == nullptr) {
        return;
    }

    QString text = label->toolTip().trimmed();
    if (text.isEmpty()) {
        text = static_cast<TrackInfoLabel *>(label)->fullText().trimmed();
    }

    QMenu menu(this);
    QAction *copy = menu.addAction(QStringLiteral("Copy"));
    copy->setEnabled(!text.isEmpty());
    QAction *configure = menu.addAction(QStringLiteral("Configure track information..."));
    const QAction *selected = menu.exec(label->mapToGlobal(pos));
    if (selected == copy) {
        QGuiApplication::clipboard()->setText(text);
    } else if (selected == configure) {
        configureTrackInfoPanel(this);
    }
}

#include "RightSidebar.moc"
