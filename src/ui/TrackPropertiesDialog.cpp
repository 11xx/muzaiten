#include "ui/TrackPropertiesDialog.h"

#include "core/Rating.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLocale>
#include <QPalette>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString escape(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return out;
}

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("—");
    }
    return QLocale().formattedDataSize(bytes, 2, QLocale::DataSizeTraditionalFormat);
}

QString humanDuration(qint64 ms)
{
    if (ms <= 0) {
        return QStringLiteral("—");
    }
    const qint64 totalSeconds = ms / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString humanTimestamp(qint64 epochSeconds)
{
    if (epochSeconds <= 0) {
        return QStringLiteral("—");
    }
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(epochSeconds), QLocale::ShortFormat);
}

QString ratingText(int rating0To100)
{
    if (rating0To100 == Rating::unset) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1 / 100").arg(rating0To100);
}

// Builds an HTML two-column table for a labelled section. Rows whose value is
// empty/placeholder are dropped so the view stays tight, like a file manager's
// properties pane.
QString section(const QString &title, const QVector<QPair<QString, QString>> &rows,
                const QString &textColor, const QString &labelColor)
{
    QString body;
    for (const auto &row : rows) {
        if (row.second.isEmpty() || row.second == QStringLiteral("—")) {
            continue;
        }
        body += QStringLiteral(
                    "<tr><td style='padding:2px 14px 2px 0; color:%3; "
                    "white-space:nowrap; vertical-align:top'>%1</td>"
                    "<td style='padding:2px 0; color:%4'>%2</td></tr>")
                    .arg(escape(row.first), escape(row.second), labelColor, textColor);
    }
    if (body.isEmpty()) {
        return QString();
    }
    return QStringLiteral("<h3 style='margin:14px 0 4px 0; color:%3'>%1</h3>"
                          "<table cellspacing='0' cellpadding='0'>%2</table>")
        .arg(escape(title), body, textColor);
}

} // namespace

TrackPropertiesDialog::TrackPropertiesDialog(const Track &track,
                                             const MetadataBlob::FullMetadata &metadata,
                                             QWidget *parent)
    : QDialog(parent)
    , m_track(track)
    , m_metadata(metadata)
{
    setWindowTitle(QStringLiteral("Properties"));
    setModal(false);
    resize(520, 600);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_view = new QTextBrowser(this);
    m_view->setOpenExternalLinks(false);
    m_view->setFrameShape(QFrame::NoFrame);
    layout->addWidget(m_view, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);

    rebuild();
}

void TrackPropertiesDialog::setExtraSection(const QString &title,
                                            const QVector<QPair<QString, QString>> &rows)
{
    m_extraTitle = title;
    m_extraRows = rows;
    rebuild();
}

void TrackPropertiesDialog::rebuild()
{
    const Track &t = m_track;
    const QPalette pal = m_view->palette();
    const QString textColor = pal.color(QPalette::Text).name();
    const QString labelColor = pal.color(QPalette::Disabled, QPalette::Text).name();
    const QString baseColor = pal.color(QPalette::Base).name();

    QString html = QStringLiteral("<div style='font-family:sans-serif; color:%1; background:%2'>")
                       .arg(textColor, baseColor);
    html += QStringLiteral("<h2 style='margin:0; color:%2'>%1</h2>")
                .arg(escape(t.title.isEmpty() ? t.filename : t.title), textColor);
    const QString subtitle = [&]() {
        QStringList parts;
        if (!t.artistName.isEmpty()) parts << t.artistName;
        if (!t.albumTitle.isEmpty()) parts << t.albumTitle;
        return parts.join(QStringLiteral("  ·  "));
    }();
    if (!subtitle.isEmpty()) {
        html += QStringLiteral("<p style='margin:2px 0 0 0; color:%2'>%1</p>")
                    .arg(escape(subtitle), labelColor);
    }

    QString audioCodec = m_metadata.codec.isEmpty() ? t.codec : m_metadata.codec;
    const int sampleRate = m_metadata.sampleRateHz > 0 ? m_metadata.sampleRateHz : t.sampleRateHz;
    const int bitrate = m_metadata.bitrateKbps > 0 ? m_metadata.bitrateKbps : t.bitrateKbps;
    const int channels = m_metadata.channels > 0 ? m_metadata.channels : t.channels;
    const int bitDepth = m_metadata.bitDepth > 0 ? m_metadata.bitDepth : t.bitDepth;

    html += section(QStringLiteral("Audio"), {
        {QStringLiteral("Duration"), humanDuration(t.durationMs)},
        {QStringLiteral("Codec"), audioCodec.toUpper()},
        {QStringLiteral("Sample rate"), sampleRate > 0
             ? QStringLiteral("%1 Hz").arg(QLocale().toString(sampleRate)) : QString()},
        {QStringLiteral("Bit depth"), bitDepth > 0
             ? QStringLiteral("%1-bit").arg(bitDepth) : QString()},
        {QStringLiteral("Bitrate"), bitrate > 0
             ? QStringLiteral("%1 kbps").arg(bitrate) : QString()},
        {QStringLiteral("Channels"), channels > 0 ? QString::number(channels) : QString()},
    }, textColor, labelColor);

    html += section(QStringLiteral("Library"), {
        {QStringLiteral("Album artist"), t.albumArtistName},
        {QStringLiteral("Track"), t.trackNumber > 0
             ? (t.trackTotal > 0 ? QStringLiteral("%1 / %2").arg(t.trackNumber).arg(t.trackTotal)
                                  : QString::number(t.trackNumber))
             : QString()},
        {QStringLiteral("Disc"), t.discNumber > 0
             ? (t.discTotal > 0 ? QStringLiteral("%1 / %2").arg(t.discNumber).arg(t.discTotal)
                                 : QString::number(t.discNumber))
             : QString()},
        {QStringLiteral("Date"), t.date},
        {QStringLiteral("Original date"), t.originalDate},
        {QStringLiteral("Rating"), ratingText(t.effectiveRating0To100)},
        {QStringLiteral("Play count"), t.playCount > 0 ? QString::number(t.playCount) : QString()},
    }, textColor, labelColor);

    html += section(QStringLiteral("File"), {
        {QStringLiteral("Path"), t.path},
        {QStringLiteral("Size"), humanSize(t.fileSize)},
        {QStringLiteral("Modified"), humanTimestamp(t.fileMtime)},
    }, textColor, labelColor);

    if (!m_extraRows.isEmpty()) {
        html += section(m_extraTitle.isEmpty() ? QStringLiteral("Playlist") : m_extraTitle,
                        m_extraRows, textColor, labelColor);
    }

    // Every captured tag, sorted, so power users can inspect Picard/MusicBrainz
    // and custom fields not promoted onto the Track struct.
    if (!m_metadata.tags.isEmpty()) {
        QVector<QPair<QString, QString>> tagRows;
        tagRows.reserve(static_cast<int>(m_metadata.tags.size()));
        for (auto it = m_metadata.tags.constBegin(); it != m_metadata.tags.constEnd(); ++it) {
            tagRows.append({it.key(), it.value().join(QStringLiteral("; "))});
        }
        std::sort(tagRows.begin(), tagRows.end(),
                  [](const auto &a, const auto &b) {
                      return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
                  });
        html += section(QStringLiteral("All tags"), tagRows, textColor, labelColor);
    } else if (!t.scanError.isEmpty()) {
        html += section(QStringLiteral("Scan"), {{QStringLiteral("Error"), t.scanError}},
                        textColor, labelColor);
    }

    html += QStringLiteral("</div>");
    m_view->setHtml(html);
}
