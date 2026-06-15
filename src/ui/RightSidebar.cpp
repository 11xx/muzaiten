#include "ui/RightSidebar.h"

#include "ui/AlbumArtFallback.h"
#include "ui/AlbumArtView.h"
#include "ui/QueueStore.h"

#include <QAction>
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
        {QStringLiteral("sampleRate"), QStringLiteral("Sample rate"), true, QStringLiteral("notable")},
        {QStringLiteral("bitDepth"), QStringLiteral("Bit depth"), true, QStringLiteral("notable")},
        {QStringLiteral("channels"), QStringLiteral("Channels"), true, QStringLiteral("notable")},
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

bool metadataItemPassesMode(const Track &track, const QString &key, const QString &mode)
{
    if (mode == QStringLiteral("notable")) {
        if (key == QStringLiteral("sampleRate")) {
            return track.sampleRateHz > 48000;
        }
        if (key == QStringLiteral("bitDepth")) {
            return track.bitDepth > 16;
        }
        if (key == QStringLiteral("channels")) {
            return track.channels > 2;
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
        if (!metadataItemPassesMode(track, key, mode)) {
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
    if (separator == QStringLiteral("|")) {
        return QStringLiteral("Pipe");
    }
    if (separator == QString::fromUtf8("\xc2\xb7")) {
        return QStringLiteral("Middle dot");
    }
    if (separator == QStringLiteral("/")) {
        return QStringLiteral("Slash");
    }
    if (separator == QStringLiteral("-")) {
        return QStringLiteral("Dash");
    }
    return QStringLiteral("Custom");
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
    static_cast<TrackInfoLabel *>(m_trackInfoProperties)->setFullText(metadataText(m_currentTrack,
                                                                                  m_trackInfoMetadataItems,
                                                                                  m_trackInfoMetadataSeparator,
                                                                                  m_trackInfoMetadataSpacing));
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

    auto *metadataTable = new QTableWidget(0, 3, &dialog);
    metadataTable->setHorizontalHeaderLabels({
        QStringLiteral("Show"),
        QStringLiteral("Metadata"),
        QStringLiteral("When"),
    });
    metadataTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    metadataTable->verticalHeader()->setVisible(false);
    metadataTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    metadataTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(metadataTable);

    for (const QJsonValue &value : trackInfoMetadataSettingsJson()) {
        const QJsonObject item = value.toObject();
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
        metadataTable->setItem(row, 1, itemCell);

        auto *mode = new QComboBox(metadataTable);
        mode->addItem(QStringLiteral("Always"), QStringLiteral("always"));
        mode->addItem(QStringLiteral("Only if notable"), QStringLiteral("notable"));
        mode->addItem(QStringLiteral("Lossy bitrate"), QStringLiteral("lossy"));
        mode->setCurrentIndex(std::max(0, mode->findData(item.value(QStringLiteral("mode")).toString(metadataDefaultMode(key)))));
        metadataTable->setCellWidget(row, 2, mode);
    }

    auto *metadataButtonRow = new QHBoxLayout;
    auto *metadataUp = new QPushButton(QStringLiteral("Up"), &dialog);
    auto *metadataDown = new QPushButton(QStringLiteral("Down"), &dialog);
    metadataButtonRow->addWidget(metadataUp);
    metadataButtonRow->addWidget(metadataDown);
    metadataButtonRow->addStretch(1);
    layout->addLayout(metadataButtonRow);

    auto *metadataOptionRow = new QHBoxLayout;
    metadataOptionRow->addWidget(new QLabel(QStringLiteral("Separator"), &dialog));
    auto *separatorPreset = new QComboBox(&dialog);
    separatorPreset->addItem(QStringLiteral("None"), QString());
    separatorPreset->addItem(QStringLiteral("Middle dot"), QString::fromUtf8("\xc2\xb7"));
    separatorPreset->addItem(QStringLiteral("Pipe"), QStringLiteral("|"));
    separatorPreset->addItem(QStringLiteral("Slash"), QStringLiteral("/"));
    separatorPreset->addItem(QStringLiteral("Dash"), QStringLiteral("-"));
    separatorPreset->addItem(QStringLiteral("Custom"), QString());
    separatorPreset->setCurrentText(separatorPresetLabel(m_trackInfoMetadataSeparator));
    auto *separatorCustom = new QLineEdit(&dialog);
    separatorCustom->setMaximumWidth(80);
    separatorCustom->setText(m_trackInfoMetadataSeparator);
    separatorCustom->setEnabled(separatorPreset->currentText() == QStringLiteral("Custom"));
    metadataOptionRow->addWidget(separatorPreset);
    metadataOptionRow->addWidget(separatorCustom);
    metadataOptionRow->addWidget(new QLabel(QStringLiteral("Item spacing"), &dialog));
    auto *metadataSpacing = new QSpinBox(&dialog);
    metadataSpacing->setRange(0, 6);
    metadataSpacing->setValue(m_trackInfoMetadataSpacing);
    metadataOptionRow->addWidget(metadataSpacing);
    layout->addLayout(metadataOptionRow);

    auto *layoutOptionRow = new QHBoxLayout;
    layoutOptionRow->addWidget(new QLabel(QStringLiteral("Alignment"), &dialog));
    auto *alignment = new QComboBox(&dialog);
    alignment->addItem(QStringLiteral("Left"), QStringLiteral("left"));
    alignment->addItem(QStringLiteral("Center"), QStringLiteral("center"));
    alignment->addItem(QStringLiteral("Right"), QStringLiteral("right"));
    alignment->setCurrentIndex(std::max(0, alignment->findData(m_trackInfoAlignment)));
    layoutOptionRow->addWidget(alignment);
    layoutOptionRow->addWidget(new QLabel(QStringLiteral("Line spacing"), &dialog));
    auto *lineSpacingMode = new QComboBox(&dialog);
    lineSpacingMode->addItem(QStringLiteral("Justify"), QStringLiteral("justify"));
    lineSpacingMode->addItem(QStringLiteral("Fixed"), QStringLiteral("fixed"));
    lineSpacingMode->setCurrentIndex(std::max(0, lineSpacingMode->findData(m_trackInfoLineSpacingMode)));
    auto *lineSpacing = new QSpinBox(&dialog);
    lineSpacing->setRange(0, 16);
    lineSpacing->setValue(m_trackInfoLineSpacing);
    lineSpacing->setSuffix(QStringLiteral(" px"));
    lineSpacing->setEnabled(lineSpacingMode->currentData().toString() == QStringLiteral("fixed"));
    layoutOptionRow->addWidget(lineSpacingMode);
    layoutOptionRow->addWidget(lineSpacing);
    layout->addLayout(layoutOptionRow);

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
    auto moveSelectedMetadata = [metadataTable](int direction) {
        const int row = metadataTable->currentRow();
        const int target = row + direction;
        if (row < 0 || target < 0 || target >= metadataTable->rowCount()) {
            return;
        }
        for (int column = 0; column < metadataTable->columnCount(); ++column) {
            if (QWidget *first = metadataTable->cellWidget(row, column)) {
                metadataTable->removeCellWidget(row, column);
                QWidget *second = metadataTable->cellWidget(target, column);
                metadataTable->removeCellWidget(target, column);
                metadataTable->setCellWidget(row, column, second);
                metadataTable->setCellWidget(target, column, first);
            } else {
                QTableWidgetItem *firstItem = metadataTable->takeItem(row, column);
                QTableWidgetItem *secondItem = metadataTable->takeItem(target, column);
                metadataTable->setItem(row, column, secondItem);
                metadataTable->setItem(target, column, firstItem);
            }
        }
        metadataTable->selectRow(target);
    };
    connect(metadataUp, &QPushButton::clicked, &dialog, [moveSelectedMetadata]() {
        moveSelectedMetadata(-1);
    });
    connect(metadataDown, &QPushButton::clicked, &dialog, [moveSelectedMetadata]() {
        moveSelectedMetadata(1);
    });
    connect(separatorPreset, &QComboBox::currentTextChanged, &dialog, [separatorPreset, separatorCustom]() {
        const bool custom = separatorPreset->currentText() == QStringLiteral("Custom");
        separatorCustom->setEnabled(custom);
        if (!custom) {
            separatorCustom->setText(separatorPreset->currentData().toString());
        }
    });
    connect(lineSpacingMode, &QComboBox::currentIndexChanged, &dialog, [lineSpacingMode, lineSpacing]() {
        lineSpacing->setEnabled(lineSpacingMode->currentData().toString() == QStringLiteral("fixed"));
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
    QJsonArray metadataItems;
    for (int row = 0; row < metadataTable->rowCount(); ++row) {
        QJsonObject item;
        item.insert(QStringLiteral("key"), metadataTable->item(row, 1)->data(Qt::UserRole).toString());
        auto *showCell = metadataTable->cellWidget(row, 0);
        auto *show = showCell == nullptr ? nullptr : showCell->findChild<QCheckBox *>();
        auto *mode = qobject_cast<QComboBox *>(metadataTable->cellWidget(row, 2));
        item.insert(QStringLiteral("visible"), show == nullptr || show->isChecked());
        item.insert(QStringLiteral("mode"), mode == nullptr ? QStringLiteral("always") : mode->currentData().toString());
        metadataItems.append(item);
    }
    root.insert(QStringLiteral("trackInfoMetadataItems"), metadataItems);
    root.insert(QStringLiteral("trackInfoMetadataSeparator"), separatorCustom->text());
    root.insert(QStringLiteral("trackInfoMetadataSpacing"), metadataSpacing->value());
    root.insert(QStringLiteral("trackInfoAlignment"), alignment->currentData().toString());
    root.insert(QStringLiteral("trackInfoLineSpacingMode"), lineSpacingMode->currentData().toString());
    root.insert(QStringLiteral("trackInfoLineSpacing"), lineSpacing->value());
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
    const QString overflowMode = m_trackInfoTitle->property("muzaitenOverflowMode").toString();
    root.insert(QStringLiteral("trackInfoOverflowMode"), overflowMode.isEmpty() ? QStringLiteral("scroll") : overflowMode);
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
    applyTrackInfoSettingsJson(root);
}

void RightSidebar::resetViewSettings()
{
    m_queueTable->resetViewSettings();
    m_splitter->setSizes({420, 150, 240});
    setTrackInfoVisible(true);
    m_trackInfoMetadataItems = defaultTrackInfoMetadataItems();
    m_trackInfoMetadataSeparator = QString::fromUtf8("\xc2\xb7");
    m_trackInfoMetadataSpacing = 1;
    m_trackInfoAlignment = QStringLiteral("left");
    m_trackInfoLineSpacingMode = QStringLiteral("justify");
    m_trackInfoLineSpacing = 1;
    const QMap<QString, QWidget *> labels = {
        {QStringLiteral("title"), m_trackInfoTitle},
        {QStringLiteral("artist"), m_trackInfoArtist},
        {QStringLiteral("album"), m_trackInfoAlbum},
        {QStringLiteral("date"), m_trackInfoYear},
        {QStringLiteral("metadata"), m_trackInfoProperties},
        {QStringLiteral("file"), m_trackInfoFile},
    };
    auto *layout = qobject_cast<QVBoxLayout *>(m_trackInfoPane->layout());
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
