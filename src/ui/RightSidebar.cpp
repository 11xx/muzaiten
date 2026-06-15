#include "ui/RightSidebar.h"

#include "ui/AlbumArtFallback.h"
#include "ui/AlbumArtView.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/QueueStore.h"

#include <QAction>
#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QSignalBlocker>
#include <QImage>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QClipboard>
#include <QFrame>

#include <algorithm>
#include <functional>

namespace {

QString formatDuration(qint64 durationMs);

struct TrackInfoField {
    QString key;
    QString label;
    bool visible = true;
    int opacity = 50;
    int sizeDelta = 0;
};

struct TrackInfoMetadataSpec {
    QString key;
    QString label;
    bool defaultVisible = true;
    QString defaultMode;
    // Threshold above which the value counts as "notable" (mode == "notable"),
    // expressed in the unit shown to the user: kHz, bits, or channel count. Zero
    // means the item has no configurable notable threshold.
    int defaultNotableMin = 0;
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

QVector<TrackInfoMetadataSpec> availableTrackInfoMetadataItems()
{
    return {
        {QStringLiteral("format"), QStringLiteral("Format"), true, QStringLiteral("always")},
        {QStringLiteral("duration"), QStringLiteral("Duration"), true, QStringLiteral("always")},
        {QStringLiteral("size"), QStringLiteral("File size"), true, QStringLiteral("always")},
        {QStringLiteral("sampleRate"), QStringLiteral("Sample rate"), true, QStringLiteral("notable"), 48},
        {QStringLiteral("bitDepth"), QStringLiteral("Bit depth"), true, QStringLiteral("notable"), 16},
        {QStringLiteral("channels"), QStringLiteral("Channels"), true, QStringLiteral("notable"), 2},
        {QStringLiteral("bitrate"), QStringLiteral("Bitrate"), true, QStringLiteral("lossy")},
        {QStringLiteral("codec"), QStringLiteral("Codec"), false, QStringLiteral("always")},
    };
}

QString metadataLabel(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return spec.label;
        }
    }
    return key;
}

QString metadataDefaultMode(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return spec.defaultMode;
        }
    }
    return QStringLiteral("always");
}

// Items whose "Only if notable" rule compares a measured value against a
// user-editable threshold. Other items have a fixed notable rule (or none).
bool metadataNotableConfigurable(const QString &key)
{
    return key == QStringLiteral("sampleRate")
        || key == QStringLiteral("bitDepth")
        || key == QStringLiteral("channels");
}

int metadataDefaultNotableMin(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return spec.defaultNotableMin;
        }
    }
    return 0;
}

bool isKnownMetadataItem(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return true;
        }
    }
    return false;
}

QJsonArray defaultTrackInfoMetadataItems()
{
    QJsonArray items;
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        QJsonObject item;
        item.insert(QStringLiteral("key"), spec.key);
        item.insert(QStringLiteral("visible"), spec.defaultVisible);
        item.insert(QStringLiteral("mode"), spec.defaultMode);
        item.insert(QStringLiteral("notableMin"), spec.defaultNotableMin);
        items.append(item);
    }
    return items;
}

QJsonArray normalizedMetadataItems(const QJsonArray &source)
{
    QJsonArray items;
    QStringList seen;
    for (const QJsonValue &value : source) {
        const QJsonObject object = value.toObject();
        const QString key = object.value(QStringLiteral("key")).toString();
        if (!isKnownMetadataItem(key) || seen.contains(key)) {
            continue;
        }
        QJsonObject item;
        item.insert(QStringLiteral("key"), key);
        item.insert(QStringLiteral("visible"), object.value(QStringLiteral("visible")).toBool(true));
        const QString mode = object.value(QStringLiteral("mode")).toString(metadataDefaultMode(key));
        item.insert(QStringLiteral("mode"), mode.isEmpty() ? metadataDefaultMode(key) : mode);
        item.insert(QStringLiteral("notableMin"),
                    object.value(QStringLiteral("notableMin")).toInt(metadataDefaultNotableMin(key)));
        items.append(item);
        seen.push_back(key);
    }
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (seen.contains(spec.key)) {
            continue;
        }
        QJsonObject item;
        item.insert(QStringLiteral("key"), spec.key);
        item.insert(QStringLiteral("visible"), spec.defaultVisible);
        item.insert(QStringLiteral("mode"), spec.defaultMode);
        item.insert(QStringLiteral("notableMin"), spec.defaultNotableMin);
        items.append(item);
    }
    return items;
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

    void setTextAlignment(Qt::Alignment alignment)
    {
        if (m_textAlignment == alignment) {
            return;
        }
        m_textAlignment = alignment;
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
            painter.drawText(alignedTextX(fm.horizontalAdvance(text)), baseline, text);
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
        return QRect(alignedTextX(visibleWidth),
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

    int alignedTextX(int textWidth) const
    {
        const QRect area = contentsRect();
        if (m_textAlignment & Qt::AlignRight) {
            return area.right() - textWidth + 1;
        }
        if (m_textAlignment & Qt::AlignHCenter) {
            return area.left() + std::max(0, (area.width() - textWidth) / 2);
        }
        return area.left();
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
    Qt::Alignment m_textAlignment = Qt::AlignLeft;
    QTimer m_scrollTimer;
    int m_scrollOffset = 0;
    int m_pauseTicksRemaining = 0;
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

QString formatSampleRate(int sampleRateHz)
{
    if (sampleRateHz <= 0) {
        return {};
    }
    if (sampleRateHz % 1000 == 0) {
        return QStringLiteral("%1 kHz").arg(sampleRateHz / 1000);
    }
    return QStringLiteral("%1 kHz").arg(QString::number(sampleRateHz / 1000.0, 'f', 1));
}

QString displayDate(const Track &track)
{
    if (!track.originalDate.isEmpty()) {
        return track.originalDate;
    }
    return track.date;
}

QString metadataValueText(const Track &track, const QString &key)
{
    if (key == QStringLiteral("format")) {
        return QFileInfo(track.path).suffix().toUpper();
    }
    if (key == QStringLiteral("duration")) {
        return formatDuration(track.durationMs);
    }
    if (key == QStringLiteral("size")) {
        return formatSize(track.fileSize);
    }
    if (key == QStringLiteral("sampleRate")) {
        return formatSampleRate(track.sampleRateHz);
    }
    if (key == QStringLiteral("bitDepth") && track.bitDepth > 0) {
        return QStringLiteral("%1-bit").arg(track.bitDepth);
    }
    if (key == QStringLiteral("channels") && track.channels > 0) {
        return QStringLiteral("%1 ch").arg(track.channels);
    }
    if (key == QStringLiteral("bitrate") && track.bitrateKbps > 0) {
        return QStringLiteral("%1 kbps").arg(track.bitrateKbps);
    }
    if (key == QStringLiteral("codec")) {
        return track.codec.toUpper();
    }
    return {};
}

bool metadataItemPassesMode(const Track &track, const QString &key, const QString &mode, int notableMin)
{
    if (mode == QStringLiteral("notable")) {
        if (key == QStringLiteral("sampleRate")) {
            return track.sampleRateHz > notableMin * 1000;
        }
        if (key == QStringLiteral("bitDepth")) {
            return track.bitDepth > notableMin;
        }
        if (key == QStringLiteral("channels")) {
            return track.channels > notableMin;
        }
    }
    if (mode == QStringLiteral("lossy")) {
        return track.bitrateKbps > 0 && track.bitDepth <= 0;
    }
    return true;
}

QString metadataJoiner(const QString &separator, int spacing)
{
    const QString spaces(std::clamp(spacing, 0, 6), QLatin1Char(' '));
    return separator.isEmpty() ? spaces : spaces + separator + spaces;
}

QString metadataText(const Track &track,
                     const QJsonArray &items,
                     const QString &separator,
                     int spacing)
{
    QStringList parts;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        if (!item.value(QStringLiteral("visible")).toBool(true)) {
            continue;
        }
        const QString key = item.value(QStringLiteral("key")).toString();
        const QString mode = item.value(QStringLiteral("mode")).toString(metadataDefaultMode(key));
        const int notableMin = item.value(QStringLiteral("notableMin")).toInt(metadataDefaultNotableMin(key));
        if (!metadataItemPassesMode(track, key, mode, notableMin)) {
            continue;
        }
        const QString text = metadataValueText(track, key).trimmed();
        if (!text.isEmpty()) {
            parts.push_back(text);
        }
    }
    return parts.join(metadataJoiner(separator, spacing));
}

Qt::Alignment trackInfoAlignment(const QString &alignment)
{
    if (alignment == QStringLiteral("center")) {
        return Qt::AlignHCenter;
    }
    if (alignment == QStringLiteral("right")) {
        return Qt::AlignRight;
    }
    return Qt::AlignLeft;
}

QString separatorPresetLabel(const QString &separator)
{
    if (separator.isEmpty()) {
        return QStringLiteral("None");
    }
    if (separator == QString::fromUtf8("\xc2\xb7")) {
        return QStringLiteral("Middle dot");
    }
    return QStringLiteral("Custom");
}

// A QTableWidget whose rows can be reordered by dragging, showing a single
// insertion line between rows (the same above/below cue the queue uses) instead
// of Qt's default cell-move drag. The actual reordering is delegated to `reorder`
// so the owner can rebuild the rows from data (rather than juggling cell-widget
// pointers, which removeCellWidget would delete out from under us).
class ReorderableTableWidget final : public QTableWidget {
public:
    ReorderableTableWidget(int rows, int columns, QWidget *parent)
        : QTableWidget(rows, columns, parent)
    {
        m_dropLine = new QWidget(viewport());
        m_dropLine->setFixedHeight(2);
        m_dropLine->setAutoFillBackground(true);
        QPalette pal = m_dropLine->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Highlight));
        m_dropLine->setPalette(pal);
        m_dropLine->hide();
    }

    std::function<void(int from, int to)> reorder;

    // The column that absorbs leftover width so the columns always span the
    // viewport, letting the user-resizable columns trade against it.
    void setFillColumn(int column) { m_fillColumn = column; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTableWidget::resizeEvent(event);
        fitFillColumn();
    }

    void fitFillColumn()
    {
        if (m_fillColumn < 0 || m_fillColumn >= columnCount()) {
            return;
        }
        int other = 0;
        for (int column = 0; column < columnCount(); ++column) {
            if (column != m_fillColumn) {
                other += columnWidth(column);
            }
        }
        const int available = viewport()->width() - other;
        if (available > 60) {
            setColumnWidth(m_fillColumn, available);
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressRow = rowAt(event->position().toPoint().y());
            m_pressY = event->position().toPoint().y();
            m_dragging = false;
        }
        QTableWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((event->buttons() & Qt::LeftButton) && m_pressRow >= 0) {
            const int y = event->position().toPoint().y();
            if (!m_dragging && std::abs(y - m_pressY) >= QApplication::startDragDistance()) {
                m_dragging = true;
            }
            if (m_dragging) {
                showDropLine(dropRowAt(y));
                return;
            }
        }
        QTableWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragging) {
            const int target = dropRowAt(event->position().toPoint().y());
            const int source = m_pressRow;
            m_dropLine->hide();
            m_dragging = false;
            m_pressRow = -1;
            if (reorder && source >= 0 && target != source && target != source + 1) {
                reorder(source, target);
            }
            return;
        }
        m_pressRow = -1;
        QTableWidget::mouseReleaseEvent(event);
    }

private:
    int dropRowAt(int y) const
    {
        if (rowCount() == 0) {
            return 0;
        }
        const int row = rowAt(y);
        if (row < 0) {
            return y < 0 ? 0 : rowCount();
        }
        const int top = rowViewportPosition(row);
        return y < top + rowHeight(row) / 2 ? row : row + 1;
    }

    void showDropLine(int row)
    {
        int y = 0;
        if (rowCount() > 0) {
            const int lastRow = rowCount() - 1;
            y = row > lastRow ? rowViewportPosition(lastRow) + rowHeight(lastRow)
                              : rowViewportPosition(row);
        }
        m_dropLine->setGeometry(0, y - 1, viewport()->width(), 2);
        m_dropLine->raise();
        m_dropLine->show();
    }

    QWidget *m_dropLine = nullptr;
    int m_pressRow = -1;
    int m_pressY = 0;
    int m_fillColumn = -1;
    bool m_dragging = false;
};

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

    m_queueTable = new QueueTable(QueueTablePreset::Sidebar, m_splitter);
    m_splitter->addWidget(m_queueTable);

    connect(m_queueTable, &QueueTable::trackActivated, this, &RightSidebar::queueTrackActivated);
    connect(m_queueTable, &QueueTable::trackRatingChanged, this, &RightSidebar::queueTrackRatingChanged);
    connect(m_queueTable, &QueueTable::rowsMoveRequested, this, &RightSidebar::queueRowsMoveRequested);
    connect(m_queueTable, &QueueTable::rowsRemoveRequested, this, &RightSidebar::queueRowsRemoveRequested);
    connect(m_queueTable, &QueueTable::removeAllMissingTracksRequested, this, &RightSidebar::removeAllMissingTracksRequested);
    connect(m_queueTable, &QueueTable::clearRequested, this, &RightSidebar::queueClearRequested);
    connect(m_queueTable, &QueueTable::clearPlayNextPriorityRequested, this, &RightSidebar::clearPlayNextPriorityRequested);
    connect(m_queueTable, &QueueTable::saveQueueAsRequested, this, &RightSidebar::saveQueueAsRequested);
    connect(m_queueTable, &QueueTable::restorePreviousQueueRequested, this, &RightSidebar::restorePreviousQueueRequested);
    connect(m_queueTable, &QueueTable::findFileRequested, this, &RightSidebar::findFileRequested);
    connect(m_queueTable, &QueueTable::propertiesRequested, this, &RightSidebar::propertiesRequested);
    connect(m_queueTable, &QueueTable::trackLibraryRequested, this, &RightSidebar::trackLibraryRequested);
    connect(m_queueTable, &QueueTable::viewSettingsChanged, this, &RightSidebar::viewSettingsChanged);

    m_trackInfoPane = new QFrame(m_splitter);
    auto *infoLayout = new QVBoxLayout(m_trackInfoPane);
    infoLayout->setContentsMargins(6, 6, 6, 6);
    infoLayout->setSpacing(1);
    m_noTrackLabel = new QLabel(QStringLiteral("No track playing"), m_trackInfoPane);
    m_noTrackLabel->setAlignment(Qt::AlignCenter);
    infoLayout->addWidget(m_noTrackLabel, 1);
    m_trackInfoTopSpacer = new QWidget(m_trackInfoPane);
    m_trackInfoTopSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_trackInfoTopSpacer->hide();
    infoLayout->addWidget(m_trackInfoTopSpacer);
    m_trackInfoTitle = valueLabel(m_trackInfoPane);
    m_trackInfoArtist = valueLabel(m_trackInfoPane);
    m_trackInfoAlbum = valueLabel(m_trackInfoPane);
    m_trackInfoYear = valueLabel(m_trackInfoPane);
    m_trackInfoProperties = valueLabel(m_trackInfoPane);
    m_trackInfoFile = valueLabel(m_trackInfoPane);
    m_trackInfoMetadataItems = defaultTrackInfoMetadataItems();
    for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
        label->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(label, &QWidget::customContextMenuRequested, this, &RightSidebar::showTrackInfoLabelMenu);
        infoLayout->addWidget(label);
    }
    m_trackInfoBottomSpacer = new QWidget(m_trackInfoPane);
    m_trackInfoBottomSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_trackInfoBottomSpacer->hide();
    infoLayout->addWidget(m_trackInfoBottomSpacer);
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
    applyTrackInfoLayoutSpacing();
    m_trackInfoPane->setMinimumHeight(96);
    m_splitter->addWidget(m_trackInfoPane);

    m_albumArt = new AlbumArtView(m_splitter);
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

void RightSidebar::setQueueStore(QueueStore *store)
{
    m_queueStore = store;
    m_queueTable->setQueueStore(store);
}

void RightSidebar::setQueue(const QVector<Track> &tracks)
{
    const int priorRow = queueCurrentRow();
    m_tracks = tracks;
    if (m_queueStore != nullptr) {
        m_queueStore->setTracks(tracks);
    }
    if (!tracks.isEmpty()) {
        const int row = priorRow >= 0 && priorRow < tracks.size()
            ? priorRow
            : (m_currentQueueIndex >= 0 && m_currentQueueIndex < tracks.size() ? m_currentQueueIndex : 0);
        setQueueCurrentRow(row);
    }
}

void RightSidebar::setPlayNextRange(int begin, int end)
{
    m_playNextBegin = begin;
    m_playNextEnd = end;
    if (m_queueStore != nullptr) {
        m_queueStore->setPlayNextRange(begin, end);
    }
}

void RightSidebar::setCurrentIndex(int index, bool reveal)
{
    // The "currently playing" row is shown with its own indicator and is kept
    // independent of the user's selection, so reordering/adding/removing never
    // hijacks the selection or scrolls the view. Only explicit playback changes
    // (reveal == true) scroll the playing row into view.
    const int rowCount = queueRowCount();
    const int current = (index >= 0 && index < rowCount) ? index : -1;
    m_currentQueueIndex = current;
    if (m_queueStore != nullptr) {
        m_queueStore->setCurrentIndex(current);
    }

    if (reveal && current >= 0) {
        m_queueTable->revealCurrentPlaying();
    }
}

void RightSidebar::setNavigationScrollPadding(int rows)
{
    if (m_queueTable != nullptr) {
        m_queueTable->setNavigationScrollPadding(rows);
    }
}

QWidget *RightSidebar::queueNavigationWidget() const
{
    return m_queueTable == nullptr ? nullptr : m_queueTable->navigationWidget();
}

int RightSidebar::queueRowCount() const
{
    return m_queueTable != nullptr ? m_queueTable->rowCount() : 0;
}

int RightSidebar::queueCurrentRow() const
{
    return m_queueTable != nullptr ? m_queueTable->currentRow() : -1;
}

void RightSidebar::setQueueCurrentRow(int row)
{
    setQueueCurrentRow(row, 0);
}

void RightSidebar::setQueueCurrentRow(int row, int scrollDirection)
{
    if (m_queueTable != nullptr) {
        m_queueTable->setCurrentRow(row, scrollDirection);
    }
}

void RightSidebar::moveQueueCurrentRow(int delta)
{
    if (queueRowCount() == 0) {
        return;
    }
    const int row = queueCurrentRow() >= 0 ? queueCurrentRow() : 0;
    setQueueCurrentRow(std::clamp(row + delta, 0, queueRowCount() - 1), delta);
}

void RightSidebar::activateCurrentQueueTrack()
{
    const int row = queueCurrentRow();
    if (row >= 0 && row < queueRowCount()) {
        m_queueTable->activateCurrentRow();
    }
}

QVector<Search::MatchDocument> RightSidebar::queueSearchDocuments() const
{
    return m_queueStore == nullptr ? QVector<Search::MatchDocument>() : m_queueStore->searchDocuments();
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
    static_cast<AlbumArtView *>(m_albumArt)->setSourcePath(effectivePath);
}

void RightSidebar::setAlbumArt(const QImage &image)
{
    if (image.isNull()) {
        setAlbumArt(QString());
        return;
    }
    m_usingArtFallback = false;
    m_albumArt->setText({});
    static_cast<AlbumArtView *>(m_albumArt)->setSourceImage(image);
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
    applyTrackInfoLayoutSpacing();
    if (!hasTrack) {
        for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
            label->setVisible(false);
        }
        return;
    }

    const QString metadata = metadataText(m_currentTrack,
                                          m_trackInfoMetadataItems,
                                          m_trackInfoMetadataSeparator,
                                          m_trackInfoMetadataSpacing);
    const QVector<QPair<QWidget *, QString>> values = {
        {m_trackInfoTitle, m_currentTrack.title.isEmpty() ? m_currentTrack.filename : m_currentTrack.title},
        {m_trackInfoArtist, m_currentTrack.artistName},
        {m_trackInfoAlbum, m_currentTrack.albumTitle},
        {m_trackInfoYear, displayDate(m_currentTrack)},
        {m_trackInfoProperties, metadata},
        {m_trackInfoFile, m_currentTrack.path},
    };
    for (const auto &[label, text] : values) {
        static_cast<TrackInfoLabel *>(label)->setFullText(text);
        // A field configured to show but with no value for this track would leave
        // a blank line in the panel, so collapse it instead of rendering empty.
        label->setVisible(!label->property("muzaitenHidden").toBool() && !text.trimmed().isEmpty());
    }
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
    dialog.resize(m_trackInfoDialogState.value(QStringLiteral("width")).toInt(680),
                  m_trackInfoDialogState.value(QStringLiteral("height")).toInt(480));
    auto *layout = new QVBoxLayout(&dialog);

    auto applyColumnWidths = [](QTableWidget *target, const QList<int> &defaults, const QJsonArray &saved) {
        for (int column = 0; column < target->columnCount(); ++column) {
            const int fallback = defaults.value(column, 80);
            const int width = column < saved.size() ? saved.at(column).toInt(fallback) : fallback;
            target->setColumnWidth(column, std::max(40, width));
        }
    };

    // Panel-wide controls first: alignment, line spacing and overflow govern the
    // whole information panel rather than any single field, so they sit above the
    // per-field and per-metadata tables.
    auto *panelOptionRow = new QHBoxLayout;
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Alignment"), &dialog));
    auto *alignment = new QComboBox(&dialog);
    alignment->addItem(QStringLiteral("Left"), QStringLiteral("left"));
    alignment->addItem(QStringLiteral("Center"), QStringLiteral("center"));
    alignment->addItem(QStringLiteral("Right"), QStringLiteral("right"));
    alignment->setCurrentIndex(std::max(0, alignment->findData(m_trackInfoAlignment)));
    panelOptionRow->addWidget(alignment);
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Line spacing"), &dialog));
    auto *lineSpacingMode = new QComboBox(&dialog);
    lineSpacingMode->addItem(QStringLiteral("Justify"), QStringLiteral("justify"));
    lineSpacingMode->addItem(QStringLiteral("Fixed"), QStringLiteral("fixed"));
    lineSpacingMode->setCurrentIndex(std::max(0, lineSpacingMode->findData(m_trackInfoLineSpacingMode)));
    panelOptionRow->addWidget(lineSpacingMode);
    auto *lineSpacing = new QSpinBox(&dialog);
    lineSpacing->setRange(0, 16);
    lineSpacing->setValue(m_trackInfoLineSpacing);
    lineSpacing->setSuffix(QStringLiteral(" px"));
    lineSpacing->setEnabled(lineSpacingMode->currentData().toString() == QStringLiteral("fixed"));
    panelOptionRow->addWidget(lineSpacing);
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Vertical"), &dialog));
    auto *verticalAlignment = new QComboBox(&dialog);
    verticalAlignment->addItem(QStringLiteral("Top"), QStringLiteral("top"));
    verticalAlignment->addItem(QStringLiteral("Center"), QStringLiteral("center"));
    verticalAlignment->addItem(QStringLiteral("Bottom"), QStringLiteral("bottom"));
    verticalAlignment->setCurrentIndex(std::max(0, verticalAlignment->findData(m_trackInfoVerticalAlignment)));
    verticalAlignment->setEnabled(lineSpacingMode->currentData().toString() == QStringLiteral("fixed"));
    panelOptionRow->addWidget(verticalAlignment);
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Overflow"), &dialog));
    auto *overflowMode = new QComboBox(&dialog);
    overflowMode->addItem(QStringLiteral("Scroll"));
    overflowMode->addItem(QStringLiteral("Truncate"));
    overflowMode->setCurrentIndex(m_trackInfoTitle->property("muzaitenOverflowMode").toString() == QStringLiteral("truncate") ? 1 : 0);
    panelOptionRow->addWidget(overflowMode);
    panelOptionRow->addStretch(1);
    layout->addLayout(panelOptionRow);

    auto *table = new ReorderableTableWidget(0, 4, &dialog);
    table->setHorizontalHeaderLabels({
        QStringLiteral("Show"),
        QStringLiteral("Field"),
        QStringLiteral("Opacity"),
        QStringLiteral("Size"),
    });
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(false);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFillColumn(1);
    NeighborColumnResizer::install(table->horizontalHeader(), [](int) { return 40; });
    layout->addWidget(table);

    const QMap<QString, QString> labels = {
        {QStringLiteral("title"), QStringLiteral("Title")},
        {QStringLiteral("artist"), QStringLiteral("Artist")},
        {QStringLiteral("album"), QStringLiteral("Album")},
        {QStringLiteral("date"), QStringLiteral("Date")},
        {QStringLiteral("metadata"), QStringLiteral("Metadata")},
        {QStringLiteral("file"), QStringLiteral("File full path")},
    };

    // Rows are rebuilt from their data on every reorder rather than swapping
    // cell-widget pointers: QTableWidget::removeCellWidget deletes the widget, so
    // shuffling pointers blanks cells and dangles. read/append/rebuild keep the
    // editable state intact.
    auto appendFieldRow = [table, labels](const QJsonObject &field) {
        const int row = table->rowCount();
        table->insertRow(row);
        const QString key = field.value(QStringLiteral("key")).toString();
        auto *show = new QCheckBox(table);
        show->setChecked(field.value(QStringLiteral("visible")).toBool(true));
        auto *showCell = new QWidget(table);
        auto *showLayout = new QHBoxLayout(showCell);
        showLayout->setContentsMargins(0, 0, 0, 0);
        showLayout->addWidget(show, 0, Qt::AlignCenter);
        table->setCellWidget(row, 0, showCell);
        auto *fieldItem = new QTableWidgetItem(labels.value(key));
        fieldItem->setData(Qt::UserRole, key);
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
    };
    auto readFieldRow = [table](int row) {
        QJsonObject field;
        field.insert(QStringLiteral("key"), table->item(row, 1)->data(Qt::UserRole).toString());
        auto *showCell = table->cellWidget(row, 0);
        auto *show = showCell == nullptr ? nullptr : showCell->findChild<QCheckBox *>();
        auto *opacity = qobject_cast<QSpinBox *>(table->cellWidget(row, 2));
        auto *size = qobject_cast<QSpinBox *>(table->cellWidget(row, 3));
        field.insert(QStringLiteral("visible"), show == nullptr || show->isChecked());
        field.insert(QStringLiteral("opacity"), opacity == nullptr ? 50 : opacity->value());
        field.insert(QStringLiteral("sizeDelta"), size == nullptr ? 0 : size->value());
        return field;
    };
    auto reorderFields = [table, appendFieldRow, readFieldRow](int from, int to) {
        QJsonArray rows;
        for (int row = 0; row < table->rowCount(); ++row) {
            rows.append(readFieldRow(row));
        }
        if (from < 0 || from >= rows.size()) {
            return;
        }
        const QJsonValue moved = rows.at(from);
        rows.removeAt(from);
        const int dest = std::clamp(to > from ? to - 1 : to, 0, static_cast<int>(rows.size()));
        rows.insert(dest, moved);
        table->setRowCount(0);
        for (const QJsonValue &value : rows) {
            appendFieldRow(value.toObject());
        }
        table->selectRow(dest);
    };

    for (const QJsonValue &value : trackInfoSettingsJson()) {
        appendFieldRow(value.toObject());
    }
    applyColumnWidths(table, {54, 240, 96, 76},
                      m_trackInfoDialogState.value(QStringLiteral("fieldCols")).toArray());

    auto *buttonRow = new QHBoxLayout;
    auto *up = new QPushButton(QStringLiteral("Up"), &dialog);
    auto *down = new QPushButton(QStringLiteral("Down"), &dialog);
    buttonRow->addWidget(up);
    buttonRow->addWidget(down);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);
    table->reorder = reorderFields;
    connect(up, &QPushButton::clicked, &dialog, [table, reorderFields]() {
        const int row = table->currentRow();
        if (row >= 0) {
            reorderFields(row, row - 1);
        }
    });
    connect(down, &QPushButton::clicked, &dialog, [table, reorderFields]() {
        const int row = table->currentRow();
        if (row >= 0) {
            reorderFields(row, row + 2);
        }
    });

    layout->addSpacing(24);

    auto *metadataTable = new ReorderableTableWidget(0, 3, &dialog);
    metadataTable->setHorizontalHeaderLabels({
        QStringLiteral("Show"),
        QStringLiteral("Metadata"),
        QStringLiteral("When"),
    });
    metadataTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    metadataTable->horizontalHeader()->setStretchLastSection(false);
    metadataTable->verticalHeader()->setVisible(false);
    metadataTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    metadataTable->setSelectionMode(QAbstractItemView::SingleSelection);
    metadataTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    metadataTable->setFillColumn(1);
    NeighborColumnResizer::install(metadataTable->horizontalHeader(), [](int) { return 40; });
    layout->addWidget(metadataTable);

    auto *metadataButtonRow = new QHBoxLayout;
    auto *metadataUp = new QPushButton(QStringLiteral("Up"), &dialog);
    auto *metadataDown = new QPushButton(QStringLiteral("Down"), &dialog);
    metadataButtonRow->addWidget(metadataUp);
    metadataButtonRow->addWidget(metadataDown);
    metadataButtonRow->addStretch(1);
    layout->addLayout(metadataButtonRow);

    // Contextual "Only if notable" customization, kept next to the separator row.
    // It edits the threshold of the currently selected metadata row and greys out
    // unless that row both supports a threshold and is set to "Only if notable".
    auto *notableRow = new QHBoxLayout;
    auto *notableLabel = new QLabel(&dialog);
    auto *notableSpin = new QSpinBox(&dialog);
    notableSpin->setMaximumWidth(110);
    notableRow->addWidget(notableLabel);
    notableRow->addWidget(notableSpin);
    notableRow->addStretch(1);
    layout->addLayout(notableRow);

    // Separator controls share one setting, so pack them tightly and left-aligned
    // rather than spreading evenly across the row.
    auto *metadataOptionRow = new QHBoxLayout;
    metadataOptionRow->addWidget(new QLabel(QStringLiteral("Separator"), &dialog));
    auto *separatorPreset = new QComboBox(&dialog);
    separatorPreset->addItem(QStringLiteral("None"), QString());
    separatorPreset->addItem(QStringLiteral("Middle dot"), QString::fromUtf8("\xc2\xb7"));
    separatorPreset->addItem(QStringLiteral("Custom"), QString());
    separatorPreset->setCurrentText(separatorPresetLabel(m_trackInfoMetadataSeparator));
    auto *separatorCustom = new QLineEdit(&dialog);
    separatorCustom->setMaximumWidth(80);
    separatorCustom->setText(m_trackInfoMetadataSeparator);
    separatorCustom->setEnabled(separatorPreset->currentText() == QStringLiteral("Custom"));
    metadataOptionRow->addWidget(separatorPreset);
    metadataOptionRow->addWidget(separatorCustom);
    metadataOptionRow->addSpacing(12);
    metadataOptionRow->addWidget(new QLabel(QStringLiteral("Item spacing"), &dialog));
    auto *metadataSpacing = new QSpinBox(&dialog);
    metadataSpacing->setRange(0, 6);
    metadataSpacing->setValue(m_trackInfoMetadataSpacing);
    metadataOptionRow->addWidget(metadataSpacing);
    metadataOptionRow->addStretch(1);
    layout->addLayout(metadataOptionRow);

    auto configureNotableSpin = [](QSpinBox *spin, const QString &key) {
        if (key == QStringLiteral("sampleRate")) {
            spin->setRange(1, 768);
            spin->setSuffix(QStringLiteral(" kHz"));
        } else if (key == QStringLiteral("bitDepth")) {
            spin->setRange(1, 64);
            spin->setSuffix(QStringLiteral("-bit"));
        } else if (key == QStringLiteral("channels")) {
            spin->setRange(1, 32);
            spin->setSuffix(QStringLiteral(" ch"));
        }
    };
    auto notablePrompt = [](const QString &key) -> QString {
        if (key == QStringLiteral("sampleRate")) {
            return QStringLiteral("Sample rate above");
        }
        if (key == QStringLiteral("bitDepth")) {
            return QStringLiteral("Bit depth above");
        }
        if (key == QStringLiteral("channels")) {
            return QStringLiteral("Channels above");
        }
        return QString();
    };
    auto refreshNotable = [=]() {
        const int row = metadataTable->currentRow();
        bool active = false;
        QString key;
        if (row >= 0 && metadataTable->item(row, 1) != nullptr) {
            key = metadataTable->item(row, 1)->data(Qt::UserRole).toString();
            auto *modeBox = qobject_cast<QComboBox *>(metadataTable->cellWidget(row, 2));
            const QString mode = modeBox == nullptr ? QString() : modeBox->currentData().toString();
            active = metadataNotableConfigurable(key) && mode == QStringLiteral("notable");
        }
        notableLabel->setText(active ? notablePrompt(key) : QStringLiteral("Notable threshold"));
        if (active) {
            const QSignalBlocker blocker(notableSpin);
            configureNotableSpin(notableSpin, key);
            notableSpin->setValue(metadataTable->item(row, 1)->data(Qt::UserRole + 1).toInt());
        }
        notableLabel->setEnabled(active);
        notableSpin->setEnabled(active);
    };

    auto *dialogPtr = &dialog;
    auto appendMetadataRow = [metadataTable, refreshNotable, dialogPtr](const QJsonObject &item) {
        const int row = metadataTable->rowCount();
        metadataTable->insertRow(row);
        auto *show = new QCheckBox(metadataTable);
        show->setChecked(item.value(QStringLiteral("visible")).toBool(true));
        auto *showCell = new QWidget(metadataTable);
        auto *showLayout = new QHBoxLayout(showCell);
        showLayout->setContentsMargins(0, 0, 0, 0);
        showLayout->addWidget(show, 0, Qt::AlignCenter);
        metadataTable->setCellWidget(row, 0, showCell);

        const QString key = item.value(QStringLiteral("key")).toString();
        auto *itemCell = new QTableWidgetItem(metadataLabel(key));
        itemCell->setData(Qt::UserRole, key);
        itemCell->setData(Qt::UserRole + 1,
                          item.value(QStringLiteral("notableMin")).toInt(metadataDefaultNotableMin(key)));
        metadataTable->setItem(row, 1, itemCell);

        auto *mode = new QComboBox(metadataTable);
        mode->addItem(QStringLiteral("Always"), QStringLiteral("always"));
        mode->addItem(QStringLiteral("Only if notable"), QStringLiteral("notable"));
        mode->addItem(QStringLiteral("Lossy bitrate"), QStringLiteral("lossy"));
        mode->setCurrentIndex(std::max(0, mode->findData(item.value(QStringLiteral("mode")).toString(metadataDefaultMode(key)))));
        metadataTable->setCellWidget(row, 2, mode);
        QObject::connect(mode, &QComboBox::currentIndexChanged, dialogPtr, [refreshNotable]() { refreshNotable(); });
    };
    auto readMetadataRow = [metadataTable](int row) {
        QJsonObject item;
        const QTableWidgetItem *cell = metadataTable->item(row, 1);
        item.insert(QStringLiteral("key"), cell->data(Qt::UserRole).toString());
        auto *showCell = metadataTable->cellWidget(row, 0);
        auto *show = showCell == nullptr ? nullptr : showCell->findChild<QCheckBox *>();
        auto *mode = qobject_cast<QComboBox *>(metadataTable->cellWidget(row, 2));
        item.insert(QStringLiteral("visible"), show == nullptr || show->isChecked());
        item.insert(QStringLiteral("mode"), mode == nullptr ? QStringLiteral("always") : mode->currentData().toString());
        item.insert(QStringLiteral("notableMin"), cell->data(Qt::UserRole + 1).toInt());
        return item;
    };
    auto reorderMetadata = [metadataTable, appendMetadataRow, readMetadataRow, refreshNotable](int from, int to) {
        QJsonArray rows;
        for (int row = 0; row < metadataTable->rowCount(); ++row) {
            rows.append(readMetadataRow(row));
        }
        if (from < 0 || from >= rows.size()) {
            return;
        }
        const QJsonValue moved = rows.at(from);
        rows.removeAt(from);
        const int dest = std::clamp(to > from ? to - 1 : to, 0, static_cast<int>(rows.size()));
        rows.insert(dest, moved);
        metadataTable->setRowCount(0);
        for (const QJsonValue &value : rows) {
            appendMetadataRow(value.toObject());
        }
        metadataTable->selectRow(dest);
        refreshNotable();
    };

    for (const QJsonValue &value : trackInfoMetadataSettingsJson()) {
        appendMetadataRow(value.toObject());
    }
    applyColumnWidths(metadataTable, {54, 220, 190},
                      m_trackInfoDialogState.value(QStringLiteral("metaCols")).toArray());

    connect(metadataTable, &QTableWidget::itemSelectionChanged, &dialog, [refreshNotable]() { refreshNotable(); });
    connect(notableSpin, &QSpinBox::valueChanged, &dialog, [metadataTable](int value) {
        const int row = metadataTable->currentRow();
        if (row < 0 || metadataTable->item(row, 1) == nullptr) {
            return;
        }
        const QString key = metadataTable->item(row, 1)->data(Qt::UserRole).toString();
        if (metadataNotableConfigurable(key)) {
            metadataTable->item(row, 1)->setData(Qt::UserRole + 1, value);
        }
    });
    refreshNotable();

    metadataTable->reorder = reorderMetadata;
    connect(metadataUp, &QPushButton::clicked, &dialog, [metadataTable, reorderMetadata]() {
        const int row = metadataTable->currentRow();
        if (row >= 0) {
            reorderMetadata(row, row - 1);
        }
    });
    connect(metadataDown, &QPushButton::clicked, &dialog, [metadataTable, reorderMetadata]() {
        const int row = metadataTable->currentRow();
        if (row >= 0) {
            reorderMetadata(row, row + 2);
        }
    });

    connect(separatorPreset, &QComboBox::currentTextChanged, &dialog, [separatorPreset, separatorCustom]() {
        const bool custom = separatorPreset->currentText() == QStringLiteral("Custom");
        separatorCustom->setEnabled(custom);
        if (!custom) {
            separatorCustom->setText(separatorPreset->currentData().toString());
        }
    });
    connect(lineSpacingMode, &QComboBox::currentIndexChanged, &dialog, [lineSpacingMode, lineSpacing, verticalAlignment]() {
        const bool fixed = lineSpacingMode->currentData().toString() == QStringLiteral("fixed");
        lineSpacing->setEnabled(fixed);
        verticalAlignment->setEnabled(fixed);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const int result = dialog.exec();

    // Remember the dialog's own UI state (size, column widths) regardless of the
    // outcome, so reopening keeps the layout the user arranged.
    auto columnWidthsOf = [](QTableWidget *target) {
        QJsonArray widths;
        for (int column = 0; column < target->columnCount(); ++column) {
            widths.append(target->columnWidth(column));
        }
        return widths;
    };
    QJsonObject dialogState;
    dialogState.insert(QStringLiteral("width"), dialog.width());
    dialogState.insert(QStringLiteral("height"), dialog.height());
    dialogState.insert(QStringLiteral("fieldCols"), columnWidthsOf(table));
    dialogState.insert(QStringLiteral("metaCols"), columnWidthsOf(metadataTable));
    m_trackInfoDialogState = dialogState;

    if (result != QDialog::Accepted) {
        emit viewSettingsChanged();
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
    QJsonArray metadataItems;
    for (int row = 0; row < metadataTable->rowCount(); ++row) {
        QJsonObject item;
        item.insert(QStringLiteral("key"), metadataTable->item(row, 1)->data(Qt::UserRole).toString());
        auto *showCell = metadataTable->cellWidget(row, 0);
        auto *show = showCell == nullptr ? nullptr : showCell->findChild<QCheckBox *>();
        auto *mode = qobject_cast<QComboBox *>(metadataTable->cellWidget(row, 2));
        item.insert(QStringLiteral("visible"), show == nullptr || show->isChecked());
        item.insert(QStringLiteral("mode"), mode == nullptr ? QStringLiteral("always") : mode->currentData().toString());
        item.insert(QStringLiteral("notableMin"), metadataTable->item(row, 1)->data(Qt::UserRole + 1).toInt());
        metadataItems.append(item);
    }
    root.insert(QStringLiteral("trackInfoMetadataItems"), metadataItems);
    root.insert(QStringLiteral("trackInfoMetadataSeparator"), separatorCustom->text());
    root.insert(QStringLiteral("trackInfoMetadataSpacing"), metadataSpacing->value());
    root.insert(QStringLiteral("trackInfoAlignment"), alignment->currentData().toString());
    root.insert(QStringLiteral("trackInfoLineSpacingMode"), lineSpacingMode->currentData().toString());
    root.insert(QStringLiteral("trackInfoLineSpacing"), lineSpacing->value());
    root.insert(QStringLiteral("trackInfoVerticalAlignment"), verticalAlignment->currentData().toString());
    root.insert(QStringLiteral("trackInfoOverflowMode"), overflowMode->currentIndex() == 1 ? QStringLiteral("truncate") : QStringLiteral("scroll"));
    applyTrackInfoSettingsJson(root);
    emit viewSettingsChanged();
}

QString RightSidebar::viewSettingsJson() const
{
    QJsonObject root = QJsonDocument::fromJson(m_queueTable->viewSettingsJson().toUtf8()).object();
    root.insert(QStringLiteral("showTrackInfo"), !m_trackInfoPane->isHidden());
    root.insert(QStringLiteral("trackInfoFields"), trackInfoSettingsJson());
    root.insert(QStringLiteral("trackInfoMetadataItems"), trackInfoMetadataSettingsJson());
    root.insert(QStringLiteral("trackInfoMetadataSeparator"), m_trackInfoMetadataSeparator);
    root.insert(QStringLiteral("trackInfoMetadataSpacing"), m_trackInfoMetadataSpacing);
    root.insert(QStringLiteral("trackInfoAlignment"), m_trackInfoAlignment);
    root.insert(QStringLiteral("trackInfoLineSpacingMode"), m_trackInfoLineSpacingMode);
    root.insert(QStringLiteral("trackInfoLineSpacing"), m_trackInfoLineSpacing);
    root.insert(QStringLiteral("trackInfoVerticalAlignment"), m_trackInfoVerticalAlignment);
    const QString overflowMode = m_trackInfoTitle->property("muzaitenOverflowMode").toString();
    root.insert(QStringLiteral("trackInfoOverflowMode"), overflowMode.isEmpty() ? QStringLiteral("scroll") : overflowMode);
    if (!m_trackInfoDialogState.isEmpty()) {
        root.insert(QStringLiteral("trackInfoDialog"), m_trackInfoDialogState);
    }
    QJsonArray splitterSizes;
    for (int size : m_splitter->sizes()) {
        splitterSizes.append(size);
    }
    root.insert(QStringLiteral("splitter"), splitterSizes);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void RightSidebar::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    m_queueTable->applyViewSettingsJson(json);
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
    m_trackInfoDialogState = root.value(QStringLiteral("trackInfoDialog")).toObject();
    applyTrackInfoSettingsJson(root);
}

void RightSidebar::resetViewSettings()
{
    m_queueTable->resetViewSettings();
    m_splitter->setSizes({420, 150, 240});
    setTrackInfoVisible(true);
    m_trackInfoMetadataItems = defaultTrackInfoMetadataItems();
    m_trackInfoDialogState = QJsonObject();
    m_trackInfoMetadataSeparator = QString::fromUtf8("\xc2\xb7");
    m_trackInfoMetadataSpacing = 1;
    m_trackInfoAlignment = QStringLiteral("left");
    m_trackInfoLineSpacingMode = QStringLiteral("justify");
    m_trackInfoLineSpacing = 1;
    m_trackInfoVerticalAlignment = QStringLiteral("top");
    const QMap<QString, QWidget *> labels = {
        {QStringLiteral("title"), m_trackInfoTitle},
        {QStringLiteral("artist"), m_trackInfoArtist},
        {QStringLiteral("album"), m_trackInfoAlbum},
        {QStringLiteral("date"), m_trackInfoYear},
        {QStringLiteral("metadata"), m_trackInfoProperties},
        {QStringLiteral("file"), m_trackInfoFile},
    };
    auto *layout = qobject_cast<QVBoxLayout *>(m_trackInfoPane->layout());
    if (layout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        layout->removeWidget(m_trackInfoBottomSpacer);
    }
    for (const TrackInfoField &field : defaultTrackInfoFields()) {
        QWidget *label = labels.value(field.key);
        if (label == nullptr) {
            continue;
        }
        label->setProperty("muzaitenHidden", !field.visible);
        label->setProperty("muzaitenOpacity", field.opacity);
        label->setProperty("muzaitenSizeDelta", field.sizeDelta);
        label->setProperty("muzaitenOverflowMode", QStringLiteral("scroll"));
        static_cast<TrackInfoLabel *>(label)->setOverflowMode(TrackInfoOverflowMode::Scroll);
        static_cast<TrackInfoLabel *>(label)->setTextAlignment(Qt::AlignLeft);
        if (layout != nullptr) {
            layout->removeWidget(label);
            layout->addWidget(label);
        }
    }
    if (layout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        layout->addWidget(m_trackInfoBottomSpacer);
    }
    restyleTrackInfoLabels();
    applyTrackInfoLayoutSpacing();
    updateTrackInfoLabels();
    emit viewSettingsChanged();
}

void RightSidebar::applyTrackInfoSettingsJson(const QJsonObject &root)
{
    const QJsonArray fields = root.value(QStringLiteral("trackInfoFields")).toArray();
    const QMap<QString, QWidget *> labels = {
        {QStringLiteral("title"), m_trackInfoTitle},
        {QStringLiteral("artist"), m_trackInfoArtist},
        {QStringLiteral("album"), m_trackInfoAlbum},
        {QStringLiteral("date"), m_trackInfoYear},
        {QStringLiteral("metadata"), m_trackInfoProperties},
        {QStringLiteral("file"), m_trackInfoFile},
    };
    auto *layout = qobject_cast<QVBoxLayout *>(m_trackInfoPane->layout());
    if (layout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        layout->removeWidget(m_trackInfoBottomSpacer);
    }
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
    if (layout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        layout->addWidget(m_trackInfoBottomSpacer);
    }
    if (root.contains(QStringLiteral("trackInfoMetadataItems"))) {
        m_trackInfoMetadataItems = normalizedMetadataItems(root.value(QStringLiteral("trackInfoMetadataItems")).toArray());
    } else if (m_trackInfoMetadataItems.isEmpty()) {
        m_trackInfoMetadataItems = defaultTrackInfoMetadataItems();
    }
    m_trackInfoMetadataSeparator = root.value(QStringLiteral("trackInfoMetadataSeparator")).toString(m_trackInfoMetadataSeparator);
    m_trackInfoMetadataSpacing = std::clamp(root.value(QStringLiteral("trackInfoMetadataSpacing")).toInt(m_trackInfoMetadataSpacing), 0, 6);
    m_trackInfoAlignment = root.value(QStringLiteral("trackInfoAlignment")).toString(m_trackInfoAlignment);
    m_trackInfoLineSpacingMode = root.value(QStringLiteral("trackInfoLineSpacingMode")).toString(m_trackInfoLineSpacingMode);
    m_trackInfoLineSpacing = std::clamp(root.value(QStringLiteral("trackInfoLineSpacing")).toInt(m_trackInfoLineSpacing), 0, 16);
    m_trackInfoVerticalAlignment = root.value(QStringLiteral("trackInfoVerticalAlignment")).toString(m_trackInfoVerticalAlignment);
    const QString overflow = root.value(QStringLiteral("trackInfoOverflowMode")).toString(QStringLiteral("scroll"));
    for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
        label->setProperty("muzaitenOverflowMode", overflow);
        static_cast<TrackInfoLabel *>(label)->setOverflowMode(overflow == QStringLiteral("truncate")
                                                                  ? TrackInfoOverflowMode::Truncate
                                                                  : TrackInfoOverflowMode::Scroll);
        static_cast<TrackInfoLabel *>(label)->setTextAlignment(trackInfoAlignment(m_trackInfoAlignment));
    }
    restyleTrackInfoLabels();
    applyTrackInfoLayoutSpacing();
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

QJsonArray RightSidebar::trackInfoMetadataSettingsJson() const
{
    return normalizedMetadataItems(m_trackInfoMetadataItems);
}

void RightSidebar::applyTrackInfoLayoutSpacing()
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_trackInfoPane->layout());
    if (layout == nullptr) {
        return;
    }
    const bool fixed = m_trackInfoLineSpacingMode == QStringLiteral("fixed");
    for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
        label->setSizePolicy(QSizePolicy::Expanding, fixed ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    }
    const bool hasTrack = !m_currentTrack.path.isEmpty();
    const bool showSpacers = fixed && hasTrack;
    if (m_trackInfoTopSpacer != nullptr) {
        m_trackInfoTopSpacer->setVisible(showSpacers
                                         && (m_trackInfoVerticalAlignment == QStringLiteral("center")
                                             || m_trackInfoVerticalAlignment == QStringLiteral("bottom")));
    }
    if (m_trackInfoBottomSpacer != nullptr) {
        m_trackInfoBottomSpacer->setVisible(showSpacers
                                            && (m_trackInfoVerticalAlignment == QStringLiteral("top")
                                                || m_trackInfoVerticalAlignment == QStringLiteral("center")));
    }
    if (m_trackInfoLineSpacingMode == QStringLiteral("fixed")) {
        layout->setSpacing(m_trackInfoLineSpacing);
        return;
    }
    layout->setSpacing(1);
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
    QJsonObject root = QJsonDocument::fromJson(m_queueTable->viewSettingsJson().toUtf8()).object();
    root.insert(QStringLiteral("headerHeight"), std::clamp(height, 18, 40));
    m_queueTable->applyViewSettingsJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

bool RightSidebar::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    Q_UNUSED(event);
    return QWidget::eventFilter(watched, event);
}

void RightSidebar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        restyleTrackInfoLabels();
        updateTrackInfoLabels();
        m_trackInfoPane->update();
        m_queueTable->update();
    }
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) && m_usingArtFallback) {
        setAlbumArt(QString());
    }
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
