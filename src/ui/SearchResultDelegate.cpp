#include "ui/SearchResultDelegate.h"

#include "core/HumanQuantity.h"
#include "search/SearchIndex.h"
#include "search/SearchRecord.h"
#include "ui/SearchResultsModel.h"

#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QStyleOptionViewItem>
#include <QTextLayout>

Q_DECLARE_METATYPE(Search::SearchRecord)

namespace {

// Convert match positions (char indices in the string) to a set for fast lookup.
QSet<int> toSet(const QVector<int> &v)
{
    QSet<int> s;
    s.reserve(v.size());
    for (int i : v) s.insert(i);
    return s;
}

} // namespace

SearchResultDelegate::SearchResultDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void SearchResultDelegate::setHoveredRow(int row)
{
    m_hoveredRow = row;
}

void SearchResultDelegate::setCurrentRow(int row)
{
    m_currentRow = row;
}

void SearchResultDelegate::setRowHeight(int height)
{
    m_rowHeight = height;
}

void SearchResultDelegate::setQuery(const Search::SearchQuery &query, bool fuzzyMode)
{
    m_query = query;
    m_fuzzyMode = fuzzyMode;
}

void SearchResultDelegate::setAddedPaths(const QSet<QString> &paths)
{
    m_addedPaths = paths;
}

QSize SearchResultDelegate::sizeHint(const QStyleOptionViewItem &option,
                                      const QModelIndex & /*index*/) const
{
    const QFontMetrics fm(option.font);
    const int lineH = fm.height();
    const int lines = 4;
    const int padding = 8;
    const int h = m_rowHeight > 0 ? m_rowHeight : (lines * lineH + padding * 2);
    return {option.rect.width(), h};
}

void SearchResultDelegate::drawHighlightedText(QPainter *painter,
                                                const QRect &rect,
                                                Qt::Alignment alignment,
                                                const QString &text,
                                                const QVector<int> &matchPositions,
                                                const QColor &normalColor,
                                                const QColor &matchColor,
                                                const QFontMetrics &fm) const
{
    if (text.isEmpty()) return;

    if (matchPositions.isEmpty()) {
        painter->setPen(normalColor);
        painter->drawText(rect, static_cast<int>(alignment) | Qt::AlignVCenter,
                          fm.elidedText(text, Qt::ElideRight, rect.width()));
        return;
    }

    // Split into runs of matched/unmatched chars and draw each segment.
    const QSet<int> matchSet = toSet(matchPositions);
    QFont boldFont = painter->font();
    boldFont.setBold(true);

    // Calculate total width to handle elision (approximate)
    int x = rect.left();
    if (alignment & Qt::AlignHCenter) {
        const int totalW = fm.horizontalAdvance(text);
        x = rect.left() + std::max(0, (rect.width() - totalW) / 2);
    } else if (alignment & Qt::AlignRight) {
        x = rect.right() - fm.horizontalAdvance(text);
    }
    const int y = rect.top() + (rect.height() + fm.ascent() - fm.descent()) / 2;
    const int maxX = rect.right();

    for (int i = 0; i < text.length() && x < maxX; ) {
        const bool isMatch = matchSet.contains(i);
        // Find the end of this run
        int j = i + 1;
        while (j < text.length() && matchSet.contains(j) == isMatch) ++j;

        const QString segment = text.mid(i, j - i);
        const int segW = fm.horizontalAdvance(segment);
        const int availW = maxX - x;
        const QString drawn = (segW > availW)
            ? fm.elidedText(segment, Qt::ElideRight, availW)
            : segment;

        painter->setPen(isMatch ? matchColor : normalColor);
        if (isMatch) {
            painter->save();
            painter->setFont(boldFont);
            painter->drawText(x, y, drawn);
            painter->restore();
        } else {
            painter->drawText(x, y, drawn);
        }
        x += fm.horizontalAdvance(drawn);
        i = j;
    }
}

void SearchResultDelegate::paint(QPainter *painter,
                                  const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    painter->save();

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    const bool selected = opt.state & QStyle::State_Selected;   // multi-select "marks"
    const bool current  = (m_currentRow == index.row());        // keyboard cursor
    const bool hovered  = (m_hoveredRow == index.row()) || (opt.state & QStyle::State_MouseOver);

    // Background: marked rows get the full Highlight fill; the cursor row (when
    // not also marked) gets a medium Highlight wash so it reads as a moving
    // "selection cursor" even while the search box keeps keyboard focus.
    if (selected) {
        painter->fillRect(opt.rect, opt.palette.color(QPalette::Highlight));
    } else if (current) {
        QColor cur = opt.palette.color(QPalette::Highlight);
        cur.setAlpha(72);
        painter->fillRect(opt.rect, cur);
    } else if (hovered) {
        QColor hover = opt.palette.color(QPalette::Highlight);
        hover.setAlpha(34);
        painter->fillRect(opt.rect, hover);
    } else if (index.row() % 2 == 1) {
        painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
    }

    // Already-added rows (playlist add mode): a green-tinted left bar plus a
    // faint wash so duplicates are obvious while scanning results.
    if (!m_addedPaths.isEmpty()) {
        const auto recVar0 = index.data(SearchResultsModel::SearchRecordRole);
        if (recVar0.isValid()
            && m_addedPaths.contains(qvariant_cast<Search::SearchRecord>(recVar0).path)) {
            QColor added(96, 170, 110);
            if (!selected && !current) {
                QColor wash = added;
                wash.setAlpha(40);
                painter->fillRect(opt.rect, wash);
            }
            painter->fillRect(QRect(opt.rect.right() - 3, opt.rect.top(), 3, opt.rect.height()), added);
        }
    }

    // Cursor accent bar on the left edge.
    if (current) {
        const QColor barColor = selected
            ? opt.palette.color(QPalette::HighlightedText)
            : opt.palette.color(QPalette::Highlight);
        painter->fillRect(QRect(opt.rect.left(), opt.rect.top(), 3, opt.rect.height()), barColor);
    }

    // Resolve record
    const auto recVar = index.data(SearchResultsModel::SearchRecordRole);
    if (!recVar.isValid()) {
        painter->restore();
        return;
    }
    const Search::SearchRecord &rec = qvariant_cast<Search::SearchRecord>(recVar);

    // Highlight positions are computed lazily here — only for the handful of
    // on-screen rows — instead of for every record during matching.
    using Search::HighlightField;
    const QVector<int> titlePos  = Search::highlightPositions(rec.title, m_query, HighlightField::Title, m_fuzzyMode);
    const QString displayArtistName = rec.albumArtistName.isEmpty() ? rec.artistName : rec.albumArtistName;
    const QVector<int> artistPos = Search::highlightPositions(displayArtistName, m_query, HighlightField::Artist, m_fuzzyMode);

    // Color palette
    const QColor primaryColor = selected
        ? opt.palette.color(QPalette::HighlightedText)
        : opt.palette.color(QPalette::Text);
    const QColor secondaryColor = selected
        ? opt.palette.color(QPalette::HighlightedText)
        : opt.palette.color(QPalette::Disabled, QPalette::Text);
    const QColor matchColor = selected
        ? opt.palette.color(QPalette::HighlightedText)  // highlight stands out less on selection
        : opt.palette.color(QPalette::Highlight);

    const QFontMetrics fm(opt.font);
    const int lineH = fm.height();
    const int pad   = 6;

    // Icon column
    constexpr int kIconW = 18;
    constexpr int kIconPad = 4;
    const int iconColW = kIconW + kIconPad * 2;
    const int textLeft = opt.rect.left() + iconColW + pad;
    const int textRight = opt.rect.right() - pad;
    const int textW = std::max(0, textRight - textLeft);

    // Compute total lines height to center vertically
    const int totalH = lineH * 4 + 3; // 4 lines + 3 gaps of 1px
    const int startY = opt.rect.top() + std::max(pad, (opt.rect.height() - totalH) / 2);

    auto lineRect = [&](int lineIdx) {
        return QRect(textLeft, startY + lineIdx * (lineH + 1), textW, lineH);
    };
    auto iconRect = [&](int lineIdx) {
        return QRect(opt.rect.left() + kIconPad,
                     startY + lineIdx * (lineH + 1),
                     kIconW, lineH);
    };

    // Use theme icons with fallback names
    static const QIcon iconTitle    = QIcon::fromTheme(QStringLiteral("audio-x-generic"),
                                       QIcon::fromTheme(QStringLiteral("media-playback-start")));
    static const QIcon iconArtist   = QIcon::fromTheme(QStringLiteral("user"),
                                       QIcon::fromTheme(QStringLiteral("system-users")));
    static const QIcon iconAlbum    = QIcon::fromTheme(QStringLiteral("media-optical"),
                                       QIcon::fromTheme(QStringLiteral("drive-optical")));
    static const QIcon iconPath     = QIcon::fromTheme(QStringLiteral("folder"),
                                       QIcon::fromTheme(QStringLiteral("inode-directory")));

    // Line 0: title  [duration]
    {
        const QString dur = humanquantity::formatClock(rec.durationMs);
        const QString titleText = dur.isEmpty() ? rec.title
            : QStringLiteral("%1  %2").arg(rec.title, dur);
        // duration is not highlighted; draw title with highlights, duration plain
        if (!iconTitle.isNull()) iconTitle.paint(painter, iconRect(0), Qt::AlignCenter,
                                                  QIcon::Normal, QIcon::Off);
        if (!dur.isEmpty() && !rec.title.isEmpty()) {
            // Draw title portion highlighted, then duration suffix plain
            const int durW = fm.horizontalAdvance(QStringLiteral("  ") + dur) + 2;
            const QRect titleOnly(textLeft, lineRect(0).top(), std::max(0, textW - durW), lineH);
            const QRect durOnly(textLeft + std::max(0, textW - durW), lineRect(0).top(), durW, lineH);
            drawHighlightedText(painter, titleOnly, Qt::AlignLeft, rec.title,
                                titlePos, primaryColor, matchColor, fm);
            painter->setPen(secondaryColor);
            painter->drawText(durOnly, Qt::AlignRight | Qt::AlignVCenter, dur);
        } else {
            drawHighlightedText(painter, lineRect(0), Qt::AlignLeft, rec.title,
                                titlePos, primaryColor, matchColor, fm);
        }
    }

    // Line 1: artist (or album artist if different from artist)
    {
        if (!iconArtist.isNull()) iconArtist.paint(painter, iconRect(1), Qt::AlignCenter,
                                                    QIcon::Normal, QIcon::Off);
        drawHighlightedText(painter, lineRect(1), Qt::AlignLeft, displayArtistName,
                            artistPos, secondaryColor, matchColor, fm);
    }

    // Line 2: album  (year)
    {
        const QString albumLine = rec.date.isEmpty() ? rec.albumTitle
            : QStringLiteral("%1  (%2)").arg(rec.albumTitle, rec.date);
        const QVector<int> albumPos = Search::highlightPositions(rec.albumTitle, m_query,
                                                                 HighlightField::Album, m_fuzzyMode);
        if (!iconAlbum.isNull()) iconAlbum.paint(painter, iconRect(2), Qt::AlignCenter,
                                                  QIcon::Normal, QIcon::Off);
        drawHighlightedText(painter, lineRect(2), Qt::AlignLeft, albumLine,
                            albumPos, secondaryColor, matchColor, fm);
    }

    // Line 3: path
    {
        const QVector<int> pathPos = Search::highlightPositions(rec.path, m_query,
                                                               HighlightField::Path, m_fuzzyMode);
        if (!iconPath.isNull()) iconPath.paint(painter, iconRect(3), Qt::AlignCenter,
                                               QIcon::Normal, QIcon::Off);
        drawHighlightedText(painter, lineRect(3), Qt::AlignLeft, rec.path,
                            pathPos, secondaryColor, matchColor, fm);
    }

    painter->restore();
}
