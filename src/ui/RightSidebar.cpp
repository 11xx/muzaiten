#include "ui/RightSidebar.h"

#include "ui/AlbumArtFallback.h"
#include "ui/DenseTableDelegate.h"

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
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QSpinBox>
#include <QTableView>
#include <QTableWidget>
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
};

constexpr ColumnSpec columns[] = {
    {"position", "#", 0},
    {"title", "Title", 1},
    {"rating", "Rating", 2},
};

constexpr auto queueRowsMimeType = "application/x-muzaiten-queue-rows";

QString ratingText(int rating0To100);

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
        return parent.isValid() ? 0 : 3;
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
            return {};
        }

        const Track &track = m_tracks.at(index.row());
        switch (index.column()) {
        case 0:
            return QString::number(index.row() + 1);
        case 1:
            return track.title;
        case 2:
            return ratingText(track.effectiveRating0To100);
        default:
            return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;
        if (index.isValid()) {
            flags |= Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        }
        return flags;
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
        endResetModel();
    }

signals:
    void rowsMoveRequested(QVector<int> rows, int destinationRow) const;

private:
    QVector<Track> m_tracks;
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
        painter.setPen(palette().color(QPalette::WindowText));

        QFont styled = font();
        styled.setUnderline(m_clickable);
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
        styled.setUnderline(m_clickable);
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
        m_pauseTicksRemaining = 24;
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
            m_pauseTicksRemaining = 18;
        }
        update();
    }

    QString m_fullText;
    bool m_clickable = false;
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

    void setSourcePixmap(const QPixmap &pixmap)
    {
        m_sourcePixmap = pixmap;
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
        if (m_sourcePixmap.isNull()) {
            setPixmap({});
            return;
        }
        const QSize target = contentsRect().size();
        if (target.width() <= 0 || target.height() <= 0) {
            setPixmap({});
            return;
        }
        setPixmap(m_sourcePixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QPixmap m_sourcePixmap;
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
    m_queueTable = new QTableView(m_splitter);
    m_queueTable->setModel(queueModel);
    m_queueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queueTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queueTable->setDragEnabled(true);
    m_queueTable->setAcceptDrops(true);
    m_queueTable->setDropIndicatorShown(true);
    m_queueTable->setDragDropMode(QAbstractItemView::InternalMove);
    m_queueTable->setDefaultDropAction(Qt::MoveAction);
    m_queueTable->setDragDropOverwriteMode(false);
    m_queueTable->setItemDelegate(new DenseTableDelegate(this));
    m_queueTable->setShowGrid(false);
    m_queueTable->setWordWrap(false);
    m_queueTable->verticalHeader()->setVisible(false);
    m_queueTable->verticalHeader()->setDefaultSectionSize(20);
    m_queueTable->verticalHeader()->setMinimumSectionSize(20);
    m_queueTable->horizontalHeader()->setFixedHeight(20);
    m_queueTable->horizontalHeader()->setSectionsMovable(true);
    m_queueTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_queueTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_queueTable->setAlternatingRowColors(true);
    m_queueTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_queueTable->setStyleSheet(QStringLiteral("QTableView::item { padding: 0 3px; }"));
    m_queueTable->viewport()->installEventFilter(this);
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
    connect(m_queueTable->horizontalHeader(), &QHeaderView::sectionResized, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(m_queueTable, &QWidget::customContextMenuRequested, this, &RightSidebar::showQueueMenu);
    connect(queueModel, &QueueTableModel::rowsMoveRequested, this, &RightSidebar::queueRowsMoveRequested);

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
}

void RightSidebar::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_queueTable->model()->rowCount()) {
        m_queueTable->clearSelection();
        return;
    }
    m_queueTable->selectRow(index);
    m_queueTable->scrollTo(m_queueTable->model()->index(index, 1), QAbstractItemView::PositionAtCenter);
}

void RightSidebar::setAlbumArt(const QString &imagePath)
{
    QPixmap pixmap(imagePath);
    if (pixmap.isNull()) {
        pixmap = QPixmap(AlbumArtFallback::resourcePath(palette()));
        if (pixmap.isNull()) {
            m_albumArt->setPixmap({});
            m_albumArt->setText(QStringLiteral("Album art"));
            return;
        }
    }

    m_albumArt->setText({});
    static_cast<AlbumArtLabel *>(m_albumArt)->setSourcePixmap(pixmap);
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
    }

    setHeaderHeight(root.value(QStringLiteral("headerHeight")).toInt(20));
    m_queueTable->verticalHeader()->setDefaultSectionSize(std::clamp(root.value(QStringLiteral("rowHeight")).toInt(20), 20, 48));
    const QByteArray headerState = QByteArray::fromBase64(root.value(QStringLiteral("headerState")).toString().toLatin1());
    if (!headerState.isEmpty()) {
        m_queueTable->horizontalHeader()->restoreState(headerState);
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
        QPalette labelPalette = palette();
        QColor color = labelPalette.color(QPalette::Text);
        color.setAlphaF(static_cast<float>(std::clamp(label->property("muzaitenOpacity").toInt() / 100.0, 0.1, 1.0)));
        labelPalette.setColor(QPalette::WindowText, color);
        labelPalette.setColor(QPalette::Text, color);
        labelPalette.setColor(QPalette::ButtonText, color);
        label->setPalette(labelPalette);
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
            const int rowHeight = std::clamp(m_queueTable->verticalHeader()->defaultSectionSize() + step, 20, 48);
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
    if (auto *denseDelegate = qobject_cast<DenseTableDelegate *>(m_queueTable->itemDelegate())) {
        denseDelegate->setHoveredRow(row);
    }
    if (previous >= 0) {
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
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange) {
        restyleTrackInfoLabels();
        updateTrackInfoLabels();
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
            emit viewSettingsChanged();
        });
    }
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
    QAction *findFile = menu.addAction(QStringLiteral("Find file"));
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
