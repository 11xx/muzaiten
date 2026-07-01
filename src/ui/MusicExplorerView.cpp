#include "ui/MusicExplorerView.h"

#include "core/HumanQuantity.h"
#include "core/TrackDisplay.h"
#include "scanner/ArtworkCache.h"
#include "ui/AlbumArtFallback.h"
#include "ui/PanelBorderStyle.h"
#include "ui/SelectionColors.h"
#include "ui/StarRating.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDynamicPropertyChangeEvent>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kCardWidth = 204;
constexpr int kCardHeight = 292;
constexpr int kArtSize = 176;
constexpr int kPanelPointerHeight = 12;
constexpr int kPanelRadius = 7;
constexpr int kPanelMarginX = 4;
constexpr int kPanelTopPadding = 18;
constexpr int kPanelBottomPadding = 10;
constexpr int kAlbumBatchTokenBase = 10000;

int yearFromAlbum(const Album &album)
{
    for (const QString &candidate : {album.originalDate, album.date}) {
        bool ok = false;
        const int year = candidate.trimmed().left(4).toInt(&ok);
        if (ok) {
            return year;
        }
    }
    return 0;
}

QString displayYearFromAlbum(const Album &album)
{
    for (const QString &candidate : {album.originalDate, album.date}) {
        const QString year = candidate.trimmed().left(4);
        if (!year.isEmpty()) {
            return year;
        }
    }
    return {};
}

Qt::Alignment alignmentFromString(const QString &value)
{
    if (value == QStringLiteral("left")) {
        return Qt::AlignLeft;
    }
    if (value == QStringLiteral("right")) {
        return Qt::AlignRight;
    }
    return Qt::AlignHCenter;
}

QColor averageColor(const QImage &image)
{
    if (image.isNull()) {
        return {};
    }
    const QImage small = image.scaled(8, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    qint64 r = 0;
    qint64 g = 0;
    qint64 b = 0;
    int count = 0;
    for (int y = 0; y < small.height(); ++y) {
        for (int x = 0; x < small.width(); ++x) {
            const QColor c = QColor::fromRgb(small.pixel(x, y));
            r += c.red();
            g += c.green();
            b += c.blue();
            ++count;
        }
    }
    return count > 0 ? QColor(int(r / count), int(g / count), int(b / count)) : QColor();
}

QColor blend(const QColor &a, const QColor &b, qreal amountB)
{
    const qreal amountA = 1.0 - amountB;
    return QColor(qRound(a.red() * amountA + b.red() * amountB),
                  qRound(a.green() * amountA + b.green() * amountB),
                  qRound(a.blue() * amountA + b.blue() * amountB));
}

QColor expandedPanelPrimaryColor(const QPalette &palette, const QColor &tint)
{
    const QColor base = palette.color(QPalette::Base);
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    QColor color = tint.isValid() ? blend(base, tint, dark ? 0.09 : 0.055) : base;
    color.setAlpha(255);
    return color;
}

QColor expandedPanelAlternateColor(const QPalette &palette, const QColor &tint)
{
    const QColor base = palette.color(QPalette::Base);
    const QColor alternate = palette.color(QPalette::AlternateBase);
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    if (!tint.isValid()) {
        return alternate == base ? blend(base, palette.color(QPalette::Window), dark ? 0.18 : 0.07) : alternate;
    }
    return blend(base, tint, dark ? 0.16 : 0.10);
}

void addRoundedPanelPath(QPainterPath *path, const QRect &panelRect, int pointerX)
{
    const int radius = std::min(kPanelRadius, std::min(panelRect.width(), panelRect.height()) / 2);
    const int px = std::clamp(pointerX, panelRect.left() + radius + 13, panelRect.right() - radius - 13);
    const int leftBase = px - 13;
    const int rightBase = px + 13;

    path->moveTo(panelRect.left() + radius, panelRect.top());
    path->lineTo(leftBase, panelRect.top());
    path->lineTo(px, 1);
    path->lineTo(rightBase, panelRect.top());
    path->lineTo(panelRect.right() - radius, panelRect.top());
    path->quadTo(panelRect.right(), panelRect.top(), panelRect.right(), panelRect.top() + radius);
    path->lineTo(panelRect.right(), panelRect.bottom() - radius);
    path->quadTo(panelRect.right(), panelRect.bottom(), panelRect.right() - radius, panelRect.bottom());
    path->lineTo(panelRect.left() + radius, panelRect.bottom());
    path->quadTo(panelRect.left(), panelRect.bottom(), panelRect.left(), panelRect.bottom() - radius);
    path->lineTo(panelRect.left(), panelRect.top() + radius);
    path->quadTo(panelRect.left(), panelRect.top(), panelRect.left() + radius, panelRect.top());
    path->closeSubpath();
}
}

class MusicExplorerView::AlbumCard final : public QWidget {
public:
    AlbumCard(MusicExplorerView *owner, int row, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
        , m_row(row)
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
    }

    void setAlbum(const Album &album)
    {
        m_album = album;
        setToolTip(QStringLiteral("%1 tracks").arg(album.trackCount));
        update();
    }

    void setArtwork(const QImage &image)
    {
        m_art = image;
        update();
    }

    void setCurrent(bool current)
    {
        if (m_current == current) return;
        m_current = current;
        update();
    }

    void setExpanded(bool expanded)
    {
        if (m_expanded == expanded) return;
        m_expanded = expanded;
        update();
    }

    void setMetrics(int cellWidth, int cellHeight, int artSize, int starSize, Qt::Alignment textAlignment)
    {
        m_artSize = artSize;
        m_starSize = starSize;
        m_textAlignment = textAlignment;
        setFixedSize(cellWidth, cellHeight);
        update();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_owner->setCurrentRow(m_row);
            QPointer<MusicExplorerView> owner(m_owner);
            QTimer::singleShot(0, m_owner, [owner]() {
                if (owner != nullptr) {
                    owner->expandCurrentAlbum(false);
                }
            });
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            emit m_owner->albumPlayReplaceRequested(QStringList{m_album.title});
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        m_owner->setCurrentRow(m_row);
        m_owner->showAlbumContextMenu(m_row, event->globalPos());
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QStyleOptionViewItem option;
        option.initFrom(this);
        option.widget = this;
        // Read selection colors from the Active group regardless of the window's
        // activation state: whether the panel reads as focused is decided by
        // isActiveMainPanel (mainPanelActive / real focus), not by the OS window
        // being active, so window (de)activation must not repaint a new color.
        option.palette.setCurrentColorGroup(QPalette::Active);
        const QRect outer = rect().adjusted(2, 2, -3, -3);
        // The current (selected) card gets a filled highlight; when it is also
        // the expanded one it keeps the softer translucent connector tint that
        // blends into the panel below.
        const bool filledSelection = m_current && !m_expanded;
        if (m_expanded) {
            QColor fill = option.palette.color(QPalette::Highlight);
            fill.setAlpha(56);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(outer, 7, 7);
        } else if (m_current) {
            // Match the library album grid: the full active highlight when this
            // panel holds focus, dimmed only when another panel is active.
            painter.setPen(Qt::NoPen);
            painter.setBrush(SelectionColors::selectedFill(option));
            painter.drawRoundedRect(outer, 7, 7);
        } else if (underMouse()) {
            QColor hover = option.palette.color(QPalette::Highlight);
            hover.setAlpha(24);
            painter.setPen(Qt::NoPen);
            painter.setBrush(hover);
            painter.drawRoundedRect(outer, 7, 7);
        }

        const int left = (width() - m_artSize) / 2;
        const QRect artRect(left, 10, m_artSize, m_artSize);
        if (!m_art.isNull()) {
            painter.drawImage(artRect, m_art);
        } else {
            QIcon(AlbumArtFallback::resourcePath(palette())).paint(&painter, artRect);
        }

        const QRect titleRect(left, artRect.bottom() + 8, m_artSize, fontMetrics().height() * 2 + 2);
        painter.setPen(filledSelection ? SelectionColors::selectedText(option)
                                        : palette().color(QPalette::Text));
        painter.drawText(titleRect, m_textAlignment | Qt::AlignTop | Qt::TextWordWrap,
                         fontMetrics().elidedText(m_album.title, Qt::ElideRight, m_artSize * 2));

        const QString year = displayYearFromAlbum(m_album);
        if (!year.isEmpty()) {
            painter.setPen(filledSelection ? SelectionColors::selectedText(option)
                                           : palette().color(QPalette::Disabled, QPalette::Text));
            painter.drawText(QRect(left, titleRect.bottom() + 2, m_artSize, fontMetrics().height()),
                             m_textAlignment | Qt::AlignVCenter, year);
        }

        StarRating::paint(&painter,
                          QRect(left + (m_artSize - (m_starSize * 5)) / 2, height() - (m_starSize + 10), m_starSize * 5, m_starSize),
                          m_album.effectiveRating0To100,
                          StarRating::unset,
                          palette(),
                          m_starSize);
    }

private:
    MusicExplorerView *m_owner = nullptr;
    Album m_album;
    QImage m_art;
    int m_row = -1;
    int m_artSize = kArtSize;
    int m_starSize = 18;
    Qt::Alignment m_textAlignment = Qt::AlignHCenter;
    bool m_current = false;
    bool m_expanded = false;
};

class MusicExplorerView::ExpandedPanel final : public QWidget {
public:
    explicit ExpandedPanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, kPanelTopPadding, 12, kPanelBottomPadding);
        layout->setSpacing(0);
    }

    void setPointerX(int x)
    {
        if (m_pointerX == x) return;
        m_pointerX = x;
        update();
    }

    int pointerX() const { return m_pointerX; }

    void setTint(const QColor &tint)
    {
        m_tint = tint;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor base = palette().color(QPalette::Base);
        const QColor window = palette().color(QPalette::Window);
        const QColor tint = m_tint;
        const bool dark = window.lightness() < 128;
        const QColor body = expandedPanelPrimaryColor(palette(), tint);
        QColor body2 = blend(base, tint, dark ? 0.055 : 0.035);
        if (!tint.isValid()) {
            body2 = blend(base, window, dark ? 0.10 : 0.04);
        }
        body2.setAlpha(255);

        const QRect panelRect = rect().adjusted(kPanelMarginX, kPanelPointerHeight, -kPanelMarginX, -1);
        QPainterPath path;
        addRoundedPanelPath(&path, panelRect, m_pointerX);

        QLinearGradient gradient(panelRect.topLeft(), panelRect.bottomRight());
        gradient.setColorAt(0.0, body);
        gradient.setColorAt(0.72, body2);
        gradient.setColorAt(1.0, blend(body2, window, dark ? 0.10 : 0.05));
        painter.fillPath(path, gradient);

        if (tint.isValid()) {
            QRadialGradient leftGlow(QPointF(panelRect.left() + panelRect.width() * 0.22,
                                             panelRect.top() + panelRect.height() * 0.18),
                                     panelRect.width() * 0.68);
            QColor glow = tint;
            glow.setAlpha(dark ? 42 : 30);
            QColor clear = tint;
            clear.setAlpha(0);
            leftGlow.setColorAt(0.0, glow);
            leftGlow.setColorAt(1.0, clear);
            painter.fillPath(path, leftGlow);

            QRadialGradient rightGlow(QPointF(panelRect.right() - panelRect.width() * 0.18,
                                              panelRect.bottom() - panelRect.height() * 0.10),
                                      panelRect.width() * 0.55);
            QColor right = blend(tint, window, dark ? 0.45 : 0.62);
            right.setAlpha(dark ? 24 : 20);
            rightGlow.setColorAt(0.0, right);
            rightGlow.setColorAt(1.0, clear);
            painter.fillPath(path, rightGlow);
        }

        QColor edge = tint.isValid()
            ? blend(palette().color(QPalette::Mid), tint, dark ? 0.18 : 0.12)
            : palette().color(QPalette::Mid);
        edge.setAlpha(dark ? 150 : 120);
        painter.setPen(QPen(edge, 1));
        painter.drawPath(path);
    }

private:
    int m_pointerX = 40;
    QColor m_tint;
};

MusicExplorerView::MusicExplorerView(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->installEventFilter(this);
    m_content = new QWidget(m_scroll);
    m_rows = new QVBoxLayout(m_content);
    m_rows->setContentsMargins(8, 8, 8, 8);
    m_rows->setSpacing(8);
    m_scroll->setWidget(m_content);
    outer->addWidget(m_scroll);

    m_inlineTrackTable = new TrackTable(this);
    m_inlineTrackTable->setChrome(TrackTableChrome::Inline);
    m_inlineTrackTable->setAutoHeightToRows(true);
    m_inlineTrackTable->installEventFilter(this);
    m_inlineTrackTable->viewport()->installEventFilter(this);

    connect(m_inlineTrackTable, &TrackTable::trackActivated, this, &MusicExplorerView::trackActivated);
    connect(m_inlineTrackTable, &TrackTable::playNextRequested, this, &MusicExplorerView::trackPlayNextRequested);
    connect(m_inlineTrackTable, &TrackTable::addToQueueRequested, this, &MusicExplorerView::trackAddToQueueRequested);
    connect(m_inlineTrackTable, &TrackTable::playNextTemporaryRequested, this, &MusicExplorerView::trackPlayNextTemporaryRequested);
    connect(m_inlineTrackTable, &TrackTable::addToQueueTemporaryRequested, this, &MusicExplorerView::trackAddToQueueTemporaryRequested);
    connect(m_inlineTrackTable, &TrackTable::addToPlaylistRequested, this, &MusicExplorerView::trackAddToPlaylistRequested);
    connect(m_inlineTrackTable, &TrackTable::findFileRequested, this, &MusicExplorerView::findFileRequested);
    connect(m_inlineTrackTable, &TrackTable::propertiesRequested, this, &MusicExplorerView::propertiesRequested);
    connect(m_inlineTrackTable, &TrackTable::trackRatingChanged, this, &MusicExplorerView::trackRatingChanged);
    connect(m_inlineTrackTable, &TrackTable::viewSettingsChanged, this, &MusicExplorerView::trackTableViewSettingsChanged);
}

void MusicExplorerView::setArtworkCache(ArtworkCache *cache)
{
    if (m_artworkCache == cache) return;
    if (m_artworkCache != nullptr) {
        disconnect(m_artworkCache, nullptr, this, nullptr);
    }
    m_artworkCache = cache;
    if (m_artworkCache != nullptr) {
        connect(m_artworkCache, &ArtworkCache::artworkReady, this, [this](const QString &token, const QImage &image, quint64 generation) {
            if (generation != quint64(m_artworkGeneration) || !token.startsWith(QStringLiteral("musicExplorer:"))) return;
            bool ok = false;
            const int row = token.mid(QStringLiteral("musicExplorer:").size()).toInt(&ok) - kAlbumBatchTokenBase;
            if (!ok || row < 0 || row >= m_albums.size()) return;
            const QString title = m_albums.at(row).title;
            m_artByTitle.insert(title, image);
            if (row < m_cards.size() && m_cards.at(row) != nullptr) {
                m_cards.at(row)->setArtwork(image);
            }
            if (row == m_expandedAlbumRow && m_expandedPanel != nullptr) {
                refreshExpandedPanelBackdrop();
            }
        });
        connect(m_artworkCache, &ArtworkCache::artworkMissing, this, [this](const QString &token, quint64 generation) {
            if (generation != quint64(m_artworkGeneration) || !token.startsWith(QStringLiteral("musicExplorer:"))) return;
            bool ok = false;
            const int row = token.mid(QStringLiteral("musicExplorer:").size()).toInt(&ok) - kAlbumBatchTokenBase;
            if (!ok || row < 0 || row >= m_albums.size()) return;
            if (row == m_expandedAlbumRow && m_expandedPanel != nullptr) {
                refreshExpandedPanelBackdrop();
            }
        });
    }
    requestVisibleArtwork();
}

void MusicExplorerView::setAlbums(const QVector<Album> &albums)
{
    const QString currentTitle = m_currentAlbumRow >= 0 && m_currentAlbumRow < m_albums.size()
        ? m_albums.at(m_currentAlbumRow).title
        : QString();
    m_sourceAlbums = albums;
    applySortedAlbums(m_sourceAlbums);
    m_artByTitle.clear();
    ++m_artworkGeneration;
    const int current = rowForTitle(currentTitle);
    if (current >= 0) {
        m_currentAlbumRow = current;
    } else if (m_currentAlbumRow >= m_albums.size()) {
        m_currentAlbumRow = m_albums.isEmpty() ? -1 : 0;
    } else if (m_currentAlbumRow < 0 && !m_albums.isEmpty()) {
        m_currentAlbumRow = 0;
    }
    const int expanded = rowForTitle(m_expandedAlbumTitle);
    m_expandedAlbumRow = expanded;
    if (expanded < 0) m_expandedAlbumTitle.clear();
    rebuildLayout();
}

void MusicExplorerView::setTrackProvider(std::function<QVector<Track>(const Album &)> provider)
{
    m_trackProvider = std::move(provider);
    refreshExpandedTracks();
}

void MusicExplorerView::setQueueIsPlaylistSourced(bool sourced)
{
    m_queueIsPlaylistSourced = sourced;
    m_inlineTrackTable->setQueueIsPlaylistSourced(sourced);
}

void MusicExplorerView::applyAlbumGridViewSettingsJson(const QString &json)
{
    if (!json.isEmpty()) {
        const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
        m_cellWidth = std::clamp(root.value(QStringLiteral("cellWidth")).toInt(kCardWidth), 160, 320);
        m_cellHeight = std::clamp(root.value(QStringLiteral("cellHeight")).toInt(kCardHeight), 240, 400);
        m_artSize = std::clamp(root.value(QStringLiteral("artSize")).toInt(kArtSize), 96, 260);
        m_spacing = std::clamp(root.value(QStringLiteral("spacing")).toInt(6), 0, 24);
        m_starSize = std::clamp(root.value(QStringLiteral("starSize")).toInt(18), 18, 28);
        m_textAlignment = alignmentFromString(root.value(QStringLiteral("textAlignment")).toString(QStringLiteral("center")));
        m_sortField = MusicSort::sortFieldFromString(root.value(QStringLiteral("sortField")).toString(), MusicSort::SortField::Year);
        m_sortDescending = root.value(QStringLiteral("sortDescending")).toBool(true);
        m_sortReverseGroups = root.value(QStringLiteral("sortReverseGroups")).toBool(false);
    }
    applySortedAlbums(m_sourceAlbums);
    rebuildLayout();
}

void MusicExplorerView::applyTrackTableViewSettingsJson(const QString &json)
{
    m_inlineTrackTable->applyViewSettingsJson(json);
}

QString MusicExplorerView::trackTableViewSettingsJson() const
{
    return m_inlineTrackTable->viewSettingsJson();
}

void MusicExplorerView::setNavigationScrollPadding(int rows)
{
    m_inlineTrackTable->setNavigationScrollPadding(rows);
}

int MusicExplorerView::rowCount() const
{
    return static_cast<int>(m_albums.size());
}

void MusicExplorerView::setCurrentRow(int row)
{
    setCurrentRowInternal(row, true);
}

void MusicExplorerView::moveCurrentByGrid(int horizontal, int vertical)
{
    if (m_albums.isEmpty()) return;
    const int row = m_currentAlbumRow >= 0 ? m_currentAlbumRow : 0;
    setCurrentRowInternal(std::clamp(row + horizontal + vertical * m_columnCount, 0, static_cast<int>(m_albums.size()) - 1), true);
}

void MusicExplorerView::expandCurrentAlbum(bool focusTracks)
{
    setExpandedAlbumRow(m_currentAlbumRow, focusTracks);
}

void MusicExplorerView::collapseExpandedAlbum()
{
    clearExpandedAlbum();
}

void MusicExplorerView::activateCurrentAlbum()
{
    expandCurrentAlbum(false);
}

QString MusicExplorerView::currentAlbumTitle() const
{
    return m_currentAlbumRow >= 0 && m_currentAlbumRow < m_albums.size()
        ? m_albums.at(m_currentAlbumRow).title
        : QString();
}

QStringList MusicExplorerView::albumTitlesForAction() const
{
    const QString title = currentAlbumTitle();
    return title.isEmpty() ? QStringList{} : QStringList{title};
}

void MusicExplorerView::addCurrentAlbumToQueue()
{
    for (const QString &title : albumTitlesForAction()) {
        emit albumAddToQueueRequested(title);
    }
}

void MusicExplorerView::addCurrentAlbumToPlaylist()
{
    emit albumAddToPlaylistRequested(albumTitlesForAction());
}

void MusicExplorerView::playNextCurrentAlbum()
{
    for (const QString &title : albumTitlesForAction()) {
        emit albumPlayNextRequested(title);
    }
}

void MusicExplorerView::selectAlbumTitle(const QString &albumTitle, bool focusTracks)
{
    const int row = rowForTitle(albumTitle);
    if (row < 0) return;
    setCurrentRowInternal(row, true);
    setExpandedAlbumRow(row, focusTracks);
}

void MusicExplorerView::selectTrackByPath(const QString &path)
{
    if (!m_expandedAlbumTitle.isEmpty()) {
        m_inlineTrackTable->selectTrackByPath(path);
        m_inlineTrackTable->setFocus(Qt::OtherFocusReason);
    }
}

int MusicExplorerView::trackRowCount() const
{
    return m_inlineTrackTable != nullptr ? m_inlineTrackTable->rowCount() : 0;
}

int MusicExplorerView::currentTrackRow() const
{
    return m_inlineTrackTable != nullptr ? m_inlineTrackTable->currentRow() : -1;
}

void MusicExplorerView::setCurrentTrackRow(int row)
{
    setCurrentTrackRow(row, 0);
}

void MusicExplorerView::setCurrentTrackRow(int row, int scrollDirection)
{
    if (m_inlineTrackTable == nullptr) {
        return;
    }
    if (m_expandedAlbumTitle.isEmpty()) {
        expandCurrentAlbum(false);
    }
    m_inlineTrackTable->setCurrentRow(row, scrollDirection);
    ensureInlineTrackVisible(scrollDirection);
}

int MusicExplorerView::expandedPanelCountForTests() const
{
    return m_expandedPanel != nullptr ? 1 : 0;
}

int MusicExplorerView::pointerXForTests() const
{
    return m_expandedPanel != nullptr ? m_expandedPanel->pointerX() : -1;
}

QVector<Search::MatchDocument> MusicExplorerView::albumSearchDocuments() const
{
    QVector<Search::MatchDocument> docs;
    docs.reserve(m_albums.size());
    for (int row = 0; row < m_albums.size(); ++row) {
        const Album &album = m_albums.at(row);
        const QString free = QStringLiteral("%1 %2 %3 %4").arg(album.title, album.albumArtistName, album.date, album.originalDate);
        QVector<Search::MatchNumeric> numeric;
        const int year = yearFromAlbum(album);
        if (year > 0) numeric.push_back({Search::TermKind::Year, year});
        if (album.effectiveRating0To100 >= 0) numeric.push_back({Search::TermKind::Rating, album.effectiveRating0To100});
        docs.push_back({row,
                        {
                            Search::makeField(Search::MatchFieldRole::Album, album.title, 400),
                            Search::makeField(Search::MatchFieldRole::AlbumArtist, album.albumArtistName, 300),
                            Search::makeField(Search::MatchFieldRole::Free, free, 100),
                        },
                        numeric});
    }
    return docs;
}

QVector<Search::MatchDocument> MusicExplorerView::trackSearchDocuments() const
{
    return m_inlineTrackTable->searchDocuments();
}

bool MusicExplorerView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scroll->viewport() && event->type() == QEvent::Resize && !m_rebuildingLayout) {
        const int columns = columnCountForWidth(availableGridWidth());
        const bool metricsChanged = recomputeEffectiveMetrics();
        if (columns != m_columnCount || metricsChanged) {
            rebuildLayoutLater();
        } else {
            updateExpandedPanelGeometry();
        }
    }
    if (m_inlineTrackTable != nullptr
        && (watched == m_inlineTrackTable || watched == m_inlineTrackTable->viewport())
        && event->type() == QEvent::KeyPress) {
        if (handleInlineTrackKey(static_cast<QKeyEvent *>(event))) {
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MusicExplorerView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int columns = columnCountForWidth(availableGridWidth());
    const bool metricsChanged = recomputeEffectiveMetrics();
    if (columns != m_columnCount || metricsChanged) {
        rebuildLayoutLater();
    } else {
        updateExpandedPanelGeometry();
    }
}

bool MusicExplorerView::event(QEvent *event)
{
    switch (event->type()) {
    // The current card's highlight follows this panel's active state (see
    // SelectionColors::isActiveMainPanel), so repaint it whenever focus moves
    // in/out or PanelSearchController toggles the mainPanelActive property. Skip
    // focus events caused by the OS window (de)activating — that is idle
    // behavior that leaves the panel's active state unchanged, so a repaint
    // would only flash the grid.
    case QEvent::FocusIn:
    case QEvent::FocusOut:
        if (static_cast<QFocusEvent *>(event)->reason() != Qt::ActiveWindowFocusReason) {
            refreshActiveHighlight();
        }
        break;
    case QEvent::DynamicPropertyChange:
        if (static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "mainPanelActive") {
            refreshActiveHighlight();
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void MusicExplorerView::refreshActiveHighlight()
{
    for (const int row : {m_currentAlbumRow, m_expandedAlbumRow}) {
        if (row >= 0 && row < m_cards.size() && m_cards.at(row) != nullptr) {
            m_cards.at(row)->update();
        }
    }
}

void MusicExplorerView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        if (m_expandedPanel != nullptr) {
            refreshExpandedPanelBackdrop();
        }
    } else if (event->type() == QEvent::ActivationChange && window()->isActiveWindow() && isVisible()) {
        // Some window managers reactivate the window without restoring keyboard
        // focus, leaving nothing focused so hjkl stop working until a click.
        // When we regain activation as the visible screen and no widget holds
        // focus, put it back on whichever panel widget last had it.
        if (QApplication::focusWidget() == nullptr) {
            if (QWidget *target = focusWidget(); target != nullptr) {
                target->setFocus(Qt::ActiveWindowFocusReason);
            }
        }
    }
}

void MusicExplorerView::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier) {
        if (event->key() == Qt::Key_J || event->key() == Qt::Key_Down) {
            moveCurrentByGrid(+1, 0);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_K || event->key() == Qt::Key_P || event->key() == Qt::Key_Up) {
            moveCurrentByGrid(-1, 0);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_H || event->key() == Qt::Key_Left) {
            if (event->key() == Qt::Key_Left) {
                moveCurrentByGrid(-1, 0);
            } else if (m_expandedPanel != nullptr) {
                clearExpandedAlbum();
            } else {
                QWidget::keyPressEvent(event);
                return;
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_L || event->key() == Qt::Key_Right) {
            if (event->key() == Qt::Key_Right) {
                moveCurrentByGrid(+1, 0);
            } else {
                expandCurrentAlbum(true);
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_N) {
            expandCurrentAlbum(event->key() == Qt::Key_N);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Space) {
            if (m_currentAlbumRow >= 0 && m_currentAlbumRow < m_albums.size()) {
                emit albumAddToQueueRequested(m_albums.at(m_currentAlbumRow).title);
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_A) {
            if (m_currentAlbumRow >= 0 && m_currentAlbumRow < m_albums.size()) {
                emit albumAddToPlaylistRequested(QStringList{m_albums.at(m_currentAlbumRow).title});
            }
            event->accept();
            return;
        }
    } else if (event->modifiers() == Qt::AltModifier && event->key() == Qt::Key_Space) {
        if (m_currentAlbumRow >= 0 && m_currentAlbumRow < m_albums.size()) {
            emit albumPlayNextRequested(m_albums.at(m_currentAlbumRow).title);
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void MusicExplorerView::rebuildLayout()
{
    if (m_rebuildingLayout) {
        return;
    }
    m_rebuildQueued = false;
    m_rebuildingLayout = true;
    const bool blockUpdates = isVisible();
    const QString expandedTitle = m_expandedAlbumTitle;
    // Preserve keyboard focus across the teardown: clearContent() reparents and
    // hides the inline track table, which otherwise drops focus (and repaints)
    // when it was the focused widget.
    QWidget *priorFocus = QApplication::focusWidget();
    const bool tracksHadFocus = priorFocus != nullptr
        && (priorFocus == m_inlineTrackTable || m_inlineTrackTable->isAncestorOf(priorFocus));
    const bool viewHadFocus = priorFocus == this;
    if (blockUpdates) {
        m_scroll->viewport()->setUpdatesEnabled(false);
        m_content->setUpdatesEnabled(false);
    }
    clearContent();
    recomputeEffectiveMetrics();
    m_columnCount = columnCountForWidth(availableGridWidth());
    m_rows->setSpacing(m_spacing);
    m_cards.resize(m_albums.size());

    for (int first = 0; first < m_albums.size(); first += m_columnCount) {
        auto *rowWidget = new QWidget(m_content);
        auto *grid = new QGridLayout(rowWidget);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(m_spacing);
        grid->setVerticalSpacing(0);
        for (int column = 0; column < m_columnCount && first + column < m_albums.size(); ++column) {
            const int albumRow = first + column;
            auto *card = new AlbumCard(this, albumRow, rowWidget);
            card->setAlbum(m_albums.at(albumRow));
            card->setMetrics(m_effectiveCellWidth, m_effectiveCellHeight, m_effectiveArtSize, m_starSize, m_textAlignment);
            const QImage art = m_artByTitle.value(m_albums.at(albumRow).title);
            if (!art.isNull()) card->setArtwork(art);
            grid->addWidget(card, 0, column, Qt::AlignTop | Qt::AlignLeft);
            m_cards[albumRow] = card;
        }
        grid->setColumnStretch(m_columnCount, 1);
        m_rows->addWidget(rowWidget);
        m_gridRows.push_back(rowWidget);
        if (isVisible()) {
            rowWidget->show();
        }

        if (!expandedTitle.isEmpty()) {
            const int expanded = rowForTitle(expandedTitle);
            if (expanded >= first && expanded < first + m_columnCount) {
                m_expandedPanel = new ExpandedPanel(m_content);
                m_expandedPanel->setUpdatesEnabled(false);
                m_inlineTrackTable->setUpdatesEnabled(false);
                m_expandedPanel->layout()->addWidget(m_inlineTrackTable);
                m_inlineTrackTable->show();
                m_rows->addWidget(m_expandedPanel);
                m_expandedAlbumRow = expanded;
                m_expandedAlbumTitle = expandedTitle;
                refreshExpandedTracks();
                refreshExpandedPanelBackdrop();
                if (isVisible()) {
                    m_expandedPanel->show();
                }
            }
        }
    }
    m_rows->addStretch(1);
    m_rows->activate();
    m_rebuildingLayout = false;
    updateCardSelection();
    updateExpandedPanelGeometry();
    if (m_expandedPanel != nullptr) {
        m_inlineTrackTable->setUpdatesEnabled(true);
        m_expandedPanel->setUpdatesEnabled(true);
        m_expandedPanel->update();
        m_inlineTrackTable->viewport()->update();
    }
    // Restore focus to the inline tracklist if it held it before the teardown
    // (clearContent() reparents and hides it, dropping focus). The view itself
    // is never reparented, so a view that had focus keeps it without help —
    // re-setting it here would fire a spurious FocusIn that re-enters
    // setActivePanel and fights an in-progress panel transition.
    if (tracksHadFocus && !viewHadFocus && m_expandedPanel != nullptr) {
        m_inlineTrackTable->setFocus(Qt::OtherFocusReason);
    }
    if (blockUpdates) {
        m_content->setUpdatesEnabled(true);
        m_scroll->viewport()->setUpdatesEnabled(true);
        m_scroll->viewport()->update();
    }
    requestVisibleArtwork();
}

void MusicExplorerView::rebuildLayoutLater()
{
    if (m_rebuildQueued) {
        return;
    }
    m_rebuildQueued = true;
    QTimer::singleShot(0, this, [this]() {
        if (m_rebuildQueued) {
            rebuildLayout();
        }
    });
}

bool MusicExplorerView::recomputeEffectiveMetrics()
{
    const int previousCellWidth = m_effectiveCellWidth;
    const int previousCellHeight = m_effectiveCellHeight;
    const int previousArtSize = m_effectiveArtSize;
    const int width = availableGridWidth();
    if (width <= 0) {
        m_effectiveCellWidth = m_cellWidth;
        m_effectiveCellHeight = m_cellHeight;
        m_effectiveArtSize = m_artSize;
    } else {
        const int columns = columnCountForWidth(width);
        const int effectiveCellWidth = m_albums.size() > columns
            ? std::max(1, (width - ((columns - 1) * m_spacing) - 1) / columns)
            : m_cellWidth;
        const int artHorizontalPadding = m_cellWidth - m_artSize;
        const int artVerticalExtra = m_cellHeight - m_artSize;
        m_effectiveCellWidth = effectiveCellWidth;
        m_effectiveArtSize = std::max(m_artSize, effectiveCellWidth - artHorizontalPadding);
        m_effectiveCellHeight = m_effectiveArtSize + artVerticalExtra;
    }
    return previousCellWidth != m_effectiveCellWidth
        || previousCellHeight != m_effectiveCellHeight
        || previousArtSize != m_effectiveArtSize;
}

void MusicExplorerView::applySortedAlbums(const QVector<Album> &albums)
{
    m_albums = albums;
    const auto direction = m_sortDescending
        ? MusicSort::SortDirection::Descending
        : MusicSort::SortDirection::Ascending;
    std::stable_sort(m_albums.begin(), m_albums.end(),
                     MusicSort::makeComparator<Album>(m_sortField, direction, m_sortReverseGroups));
}

void MusicExplorerView::clearContent()
{
    m_inlineTrackTable->setParent(this);
    m_inlineTrackTable->hide();
    m_expandedPanel = nullptr;
    m_cards.clear();
    m_gridRows.clear();
    while (QLayoutItem *item = m_rows->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

void MusicExplorerView::requestVisibleArtwork()
{
    for (int row = 0; row < m_albums.size(); ++row) {
        requestArtworkForAlbum(row);
    }
}

void MusicExplorerView::requestArtworkForAlbum(int row)
{
    if (m_artworkCache == nullptr || row < 0 || row >= m_albums.size()) return;
    const Album &album = m_albums.at(row);
    if (album.representativeDir.isEmpty()) return;
    const QString token = QStringLiteral("musicExplorer:%1").arg(kAlbumBatchTokenBase + row);
    m_artworkCache->requestArtwork(token, album.representativeDir, QString(), m_artworkGeneration);
}

void MusicExplorerView::setCurrentRowInternal(int row, bool ensureVisible)
{
    if (m_albums.isEmpty()) {
        m_currentAlbumRow = -1;
        updateCardSelection();
        return;
    }
    const int safe = std::clamp(row, 0, static_cast<int>(m_albums.size()) - 1);
    if (m_currentAlbumRow == safe) {
        return;
    }
    m_currentAlbumRow = safe;
    updateCardSelection();
    if (ensureVisible && safe < m_cards.size() && m_cards.at(safe) != nullptr) {
        m_scroll->ensureWidgetVisible(m_cards.at(safe), 24, 24);
    }
}

bool MusicExplorerView::moveExpandedPanelToRow(int row)
{
    // Fast path for switching which album is expanded while a panel already
    // exists and the grid structure is intact. Reusing the cards and inline
    // table (instead of tearing everything down in rebuildLayout) avoids the
    // flicker and focus loss that a full rebuild causes — especially after the
    // tracklist has been interacted with.
    if (m_expandedPanel == nullptr || m_columnCount <= 0 || row < 0 || row >= m_albums.size()
        || m_cards.size() != m_albums.size() || m_gridRows.isEmpty()) {
        return false;
    }
    const int gridRow = row / m_columnCount;
    if (gridRow < 0 || gridRow >= m_gridRows.size() || m_gridRows.at(gridRow) == nullptr) {
        return false;
    }

    const bool blockUpdates = isVisible();
    if (blockUpdates) {
        m_scroll->viewport()->setUpdatesEnabled(false);
        m_content->setUpdatesEnabled(false);
        m_expandedPanel->setUpdatesEnabled(false);
        m_inlineTrackTable->setUpdatesEnabled(false);
    }

    // Detach the panel from its current slot (without destroying it), then
    // reinsert it right after the grid row that holds the newly expanded album.
    m_rows->removeWidget(m_expandedPanel);
    int insertIndex = m_rows->count();
    for (int i = 0; i < m_rows->count(); ++i) {
        if (m_rows->itemAt(i)->widget() == m_gridRows.at(gridRow)) {
            insertIndex = i + 1;
            break;
        }
    }
    m_rows->insertWidget(insertIndex, m_expandedPanel);

    m_expandedAlbumRow = row;
    m_expandedAlbumTitle = m_albums.at(row).title;
    refreshExpandedTracks();
    refreshExpandedPanelBackdrop();
    m_rows->activate();
    updateCardSelection();
    updateExpandedPanelGeometry();

    if (blockUpdates) {
        m_inlineTrackTable->setUpdatesEnabled(true);
        m_expandedPanel->setUpdatesEnabled(true);
        m_expandedPanel->update();
        m_inlineTrackTable->viewport()->update();
        m_content->setUpdatesEnabled(true);
        m_scroll->viewport()->setUpdatesEnabled(true);
        m_scroll->viewport()->update();
    }
    return true;
}

void MusicExplorerView::setExpandedAlbumRow(int row, bool focusTracks)
{
    if (row < 0 || row >= m_albums.size()) return;
    if (!moveExpandedPanelToRow(row)) {
        m_expandedAlbumRow = row;
        m_expandedAlbumTitle = m_albums.at(row).title;
        rebuildLayout();
    }
    if (focusTracks) {
        m_inlineTrackTable->setFocus(Qt::OtherFocusReason);
    }
    ensureExpandedPanelVisible();
}

void MusicExplorerView::clearExpandedAlbum()
{
    if (m_expandedAlbumTitle.isEmpty() && m_expandedAlbumRow < 0) {
        return;
    }
    m_expandedAlbumRow = -1;
    m_expandedAlbumTitle.clear();
    m_inlineTrackTable->setParent(this);
    m_inlineTrackTable->hide();
    setFocus(Qt::OtherFocusReason);
    rebuildLayout();
}

void MusicExplorerView::refreshExpandedTracks()
{
    if (m_inlineTrackTable == nullptr || m_expandedAlbumTitle.isEmpty()) return;
    const int row = m_expandedAlbumRow >= 0 && m_expandedAlbumRow < m_albums.size()
        ? m_expandedAlbumRow
        : rowForTitle(m_expandedAlbumTitle);
    const QVector<Track> tracks = (m_trackProvider && row >= 0)
        ? m_trackProvider(m_albums.at(row))
        : QVector<Track>{};
    m_inlineTrackTable->setTracks(tracks);
}

void MusicExplorerView::updateCardSelection()
{
    for (int row = 0; row < m_cards.size(); ++row) {
        if (m_cards.at(row) == nullptr) continue;
        m_cards.at(row)->setCurrent(row == m_currentAlbumRow);
        m_cards.at(row)->setExpanded(row == m_expandedAlbumRow);
    }
}

void MusicExplorerView::showAlbumContextMenu(int row, const QPoint &globalPos)
{
    if (row < 0 || row >= m_albums.size()) return;
    const Album album = m_albums.at(row);
    QMenu menu(this);
    QAction *playNow = menu.addAction(QStringLiteral("Play album"));
    connect(playNow, &QAction::triggered, this, [this, album]() { emit albumPlayReplaceRequested(QStringList{album.title}); });
    QAction *playNext = menu.addAction(QStringLiteral("Play next"));
    connect(playNext, &QAction::triggered, this, [this, album]() { emit albumPlayNextRequested(album.title); });
    QAction *addQueue = menu.addAction(QStringLiteral("Add to queue"));
    connect(addQueue, &QAction::triggered, this, [this, album]() { emit albumAddToQueueRequested(album.title); });
    if (m_queueIsPlaylistSourced) {
        QAction *playNextTemp = menu.addAction(QStringLiteral("Play next (don't save to playlist)"));
        connect(playNextTemp, &QAction::triggered, this, [this, album]() { emit albumPlayNextTemporaryRequested(album.title); });
        QAction *addQueueTemp = menu.addAction(QStringLiteral("Add to queue (don't save to playlist)"));
        connect(addQueueTemp, &QAction::triggered, this, [this, album]() { emit albumAddToQueueTemporaryRequested(album.title); });
    }
    QAction *addPlaylist = menu.addAction(QStringLiteral("Add to playlist..."));
    connect(addPlaylist, &QAction::triggered, this, [this, album]() { emit albumAddToPlaylistRequested(QStringList{album.title}); });
    menu.exec(globalPos);
}

bool MusicExplorerView::handleInlineTrackKey(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_J:
        case Qt::Key_N:
        case Qt::Key_Down:
            m_inlineTrackTable->moveCurrentRow(+1);
            ensureInlineTrackVisible(+1);
            event->accept();
            return true;
        case Qt::Key_K:
        case Qt::Key_P:
        case Qt::Key_Up:
            m_inlineTrackTable->moveCurrentRow(-1);
            ensureInlineTrackVisible(-1);
            event->accept();
            return true;
        case Qt::Key_H:
        case Qt::Key_Left:
        case Qt::Key_Escape:
            if (event->key() == Qt::Key_H || event->key() == Qt::Key_Left) {
                clearExpandedAlbum();
            } else {
                setFocus(Qt::OtherFocusReason);
            }
            event->accept();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            m_inlineTrackTable->activateCurrentTrack();
            event->accept();
            return true;
        case Qt::Key_Space:
            m_inlineTrackTable->addCurrentTrackToQueue();
            event->accept();
            return true;
        case Qt::Key_A:
            m_inlineTrackTable->addCurrentTrackToPlaylist();
            event->accept();
            return true;
        default:
            break;
        }
    } else if (event->modifiers() == Qt::AltModifier && event->key() == Qt::Key_Space) {
        m_inlineTrackTable->playNextCurrentTrack();
        event->accept();
        return true;
    }
    return false;
}

int MusicExplorerView::columnCountForWidth(int width) const
{
    return std::max(1, (std::max(1, width) + m_spacing) / std::max(1, m_cellWidth + m_spacing));
}

int MusicExplorerView::rowForTitle(const QString &albumTitle) const
{
    if (albumTitle.isEmpty()) return -1;
    for (int row = 0; row < m_albums.size(); ++row) {
        if (m_albums.at(row).title == albumTitle) return row;
    }
    return -1;
}

QRect MusicExplorerView::cardGeometryInContent(int row) const
{
    if (row < 0 || row >= m_cards.size() || m_cards.at(row) == nullptr) return {};
    return QRect(m_cards.at(row)->mapTo(m_content, QPoint(0, 0)), m_cards.at(row)->size());
}

QRect MusicExplorerView::cardGeometryForTests(int row) const
{
    if (row < 0 || row >= m_cards.size() || m_cards.at(row) == nullptr) return {};
    return QRect(m_cards.at(row)->mapTo(this, QPoint(0, 0)), m_cards.at(row)->size());
}

QWidget *MusicExplorerView::cardWidgetForTests(int row) const
{
    return row >= 0 && row < m_cards.size() ? m_cards.at(row) : nullptr;
}

int MusicExplorerView::scrollValueForTests() const
{
    return m_scroll != nullptr && m_scroll->verticalScrollBar() != nullptr
        ? m_scroll->verticalScrollBar()->value()
        : 0;
}

QColor MusicExplorerView::artTintForAlbum(const QString &albumTitle) const
{
    const QColor art = averageColor(m_artByTitle.value(albumTitle));
    return art.isValid() ? art : QColor();
}

void MusicExplorerView::updateExpandedPanelGeometry()
{
    if (m_expandedPanel == nullptr) {
        return;
    }
    m_rows->activate();
    const QRect card = cardGeometryInContent(m_expandedAlbumRow);
    if (!card.isEmpty()) {
        m_expandedPanel->setPointerX(card.center().x() - m_expandedPanel->x());
    }
    refreshExpandedPanelBackdrop();
}

void MusicExplorerView::refreshExpandedPanelBackdrop()
{
    const QColor tint = artTintForAlbum(m_expandedAlbumTitle);
    if (m_expandedPanel != nullptr) {
        m_expandedPanel->setTint(tint);
    }
    applyExpandedTrackPalette(tint);
}

void MusicExplorerView::applyExpandedTrackPalette(const QColor &tint)
{
    if (m_inlineTrackTable == nullptr) {
        return;
    }

    const QPalette appPalette = palette();
    const bool dark = appPalette.color(QPalette::Window).lightness() < 128;
    const QColor rowBase = expandedPanelPrimaryColor(appPalette, tint);
    QColor alternate = expandedPanelAlternateColor(appPalette, tint);
    alternate.setAlpha(rowBase.alpha());
    // Reuse the even-row zebra shade for the header so it tracks the album tint
    // instead of standing out as a flat opaque bar like the main tracklist's.
    const QColor header = alternate;
    const QColor highlight = appPalette.color(QPalette::Highlight);
    TrackTableRowStyle rowStyle;
    rowStyle.enabled = true;
    rowStyle.primary = rowBase;
    rowStyle.alternate = alternate;
    rowStyle.hover = blend(rowBase, highlight, dark ? 0.24 : 0.16);
    rowStyle.playing = blend(rowBase, highlight, dark ? 0.28 : 0.20);
    // The active selection pairs with full HighlightedText, so use the full
    // highlight (like the main tracklist) rather than a partial blend that reads
    // as dimmed under opaque foreground text. Only the inactive selection dims.
    rowStyle.selected = highlight;
    rowStyle.inactiveSelected = blend(rowBase, highlight, dark ? 0.30 : 0.22);
    rowStyle.text = appPalette.color(QPalette::Text);
    rowStyle.selectedText = appPalette.color(QPalette::HighlightedText);
    QPalette tablePalette = m_inlineTrackTable->palette();
    tablePalette.setColor(QPalette::Base, rowBase);
    tablePalette.setColor(QPalette::AlternateBase, alternate);
    tablePalette.setColor(QPalette::Window, rowBase);
    tablePalette.setColor(QPalette::Button, header);
    m_inlineTrackTable->setPalette(tablePalette);
    m_inlineTrackTable->setRowStyle(rowStyle);
    m_inlineTrackTable->viewport()->setPalette(tablePalette);
    m_inlineTrackTable->viewport()->setAutoFillBackground(true);
    m_inlineTrackTable->horizontalHeader()->setPalette(tablePalette);
    m_inlineTrackTable->horizontalHeader()->viewport()->setPalette(tablePalette);
    m_inlineTrackTable->viewport()->update();
    m_inlineTrackTable->horizontalHeader()->viewport()->update();
}

void MusicExplorerView::ensureExpandedPanelVisible()
{
    if (m_scroll == nullptr || m_expandedPanel == nullptr || m_scroll->verticalScrollBar() == nullptr) {
        return;
    }

    m_rows->activate();
    QScrollBar *bar = m_scroll->verticalScrollBar();
    const QRect panelRect(m_expandedPanel->mapTo(m_content, QPoint(0, 0)), m_expandedPanel->size());
    if (panelRect.isEmpty()) {
        return;
    }

    constexpr int margin = 8;
    const int viewportHeight = std::max(1, m_scroll->viewport()->height());
    const int viewportTop = bar->value();
    const int viewportBottom = viewportTop + viewportHeight;
    int desired = viewportTop;

    if (panelRect.height() + margin * 2 >= viewportHeight) {
        if (panelRect.top() < viewportTop || panelRect.bottom() > viewportBottom) {
            desired = panelRect.top() - margin;
        }
    } else if (panelRect.top() - margin < viewportTop) {
        desired = panelRect.top() - margin;
    } else if (panelRect.bottom() + margin > viewportBottom) {
        desired = panelRect.bottom() + margin - viewportHeight;
    }

    desired = std::clamp(desired, bar->minimum(), bar->maximum());
    if (desired != bar->value()) {
        bar->setValue(desired);
    }
}

void MusicExplorerView::ensureInlineTrackVisible(int direction)
{
    if (m_scroll == nullptr || m_inlineTrackTable == nullptr || m_scroll->verticalScrollBar() == nullptr) {
        return;
    }
    if (m_inlineTrackTable->model() == nullptr || m_inlineTrackTable->currentRow() < 0) {
        return;
    }

    QScrollBar *bar = m_scroll->verticalScrollBar();
    const int row = m_inlineTrackTable->currentRow();
    int y = m_inlineTrackTable->horizontalHeader() != nullptr && !m_inlineTrackTable->horizontalHeader()->isHidden()
        ? m_inlineTrackTable->horizontalHeader()->height()
        : 0;
    for (int i = 0; i < row; ++i) {
        y += std::max(1, m_inlineTrackTable->rowHeight(i));
    }
    const int selectedRowHeight = std::max(1, m_inlineTrackTable->rowHeight(row));
    const QPoint rowTopLeft = m_inlineTrackTable->mapTo(m_content, QPoint(0, y));
    const QRect contentRow(rowTopLeft, QSize(m_inlineTrackTable->viewport()->width(), selectedRowHeight));
    const int rowHeight = std::max(1, contentRow.height());
    const int padding = std::max(0, m_inlineTrackTable->navigationScrollPadding()) * rowHeight;
    const int viewportHeight = std::max(1, m_scroll->viewport()->height());
    const int viewportTop = bar->value();
    const int viewportBottom = viewportTop + viewportHeight;
    int desired = viewportTop;

    if (direction > 0 && contentRow.bottom() + padding > viewportBottom) {
        desired = contentRow.bottom() + padding - viewportHeight + 1;
    } else if (direction < 0 && contentRow.top() - padding < viewportTop) {
        desired = contentRow.top() - padding;
    } else if (direction == 0) {
        if (contentRow.top() < viewportTop) {
            desired = contentRow.top();
        } else if (contentRow.bottom() > viewportBottom) {
            desired = contentRow.bottom() - viewportHeight + 1;
        }
    }

    desired = std::clamp(desired, bar->minimum(), bar->maximum());
    if (desired != bar->value()) {
        bar->setValue(desired);
    }
}

int MusicExplorerView::availableGridWidth() const
{
    if (m_scroll == nullptr || m_rows == nullptr) {
        return m_cellWidth;
    }
    const QMargins margins = m_rows->contentsMargins();
    return std::max(1, m_scroll->viewport()->width() - margins.left() - margins.right());
}
