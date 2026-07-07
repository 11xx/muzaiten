#include "ui/trackinfo/TrackInfoPanel.h"

#include "ui/trackinfo/TrackInfoSettings.h"
#include "ui/trackinfo/TrackInfoSettingsDialog.h"

#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QJsonValue>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

using namespace trackinfo;

enum class TrackInfoOverflowMode {
    Scroll,
    Truncate,
};

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

TrackInfoLabel *valueLabel(QWidget *parent)
{
    auto *label = new TrackInfoLabel(parent);
    return label;
}

} // namespace

TrackInfoPanel::TrackInfoPanel(QWidget *parent)
    : QFrame(parent)
{
    auto *infoLayout = new QVBoxLayout(this);
    infoLayout->setContentsMargins(6, 6, 6, 6);
    infoLayout->setSpacing(1);
    m_noTrackLabel = new QLabel(QStringLiteral("No track playing"), this);
    m_noTrackLabel->setAlignment(Qt::AlignCenter);
    infoLayout->addWidget(m_noTrackLabel, 1);
    m_trackInfoTopSpacer = new QWidget(this);
    m_trackInfoTopSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_trackInfoTopSpacer->hide();
    infoLayout->addWidget(m_trackInfoTopSpacer);
    m_trackInfoTitle = valueLabel(this);
    m_trackInfoArtist = valueLabel(this);
    m_trackInfoAlbum = valueLabel(this);
    m_trackInfoYear = valueLabel(this);
    m_trackInfoProperties = valueLabel(this);
    m_trackInfoFile = valueLabel(this);
    m_trackInfoMetadataItems = defaultTrackInfoMetadataItems();
    for (auto *label : {m_trackInfoTitle, m_trackInfoArtist, m_trackInfoAlbum, m_trackInfoYear, m_trackInfoProperties, m_trackInfoFile}) {
        label->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(label, &QWidget::customContextMenuRequested, this, &TrackInfoPanel::showTrackInfoLabelMenu);
        infoLayout->addWidget(label);
    }
    m_trackInfoBottomSpacer = new QWidget(this);
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
    setMinimumHeight(96);
    setTrack({});
}

void TrackInfoPanel::setTrack(const Track &track)
{
    m_currentTrack = track;
    updateTrackInfoLabels();
}

void TrackInfoPanel::updateTrackInfoLabels()
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

void TrackInfoPanel::configure(QWidget *dialogParent)
{
    const TrackInfoDialogResult result = runTrackInfoSettingsDialog(dialogParent, settings());
    m_trackInfoDialogState = result.dialogState;
    if (result.accepted) {
        applyTrackInfoSettingsJson(result.settings);
    }
    emit settingsChanged();
}

QJsonObject TrackInfoPanel::settings() const
{
    QJsonObject root;
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
    return root;
}

void TrackInfoPanel::applySettings(const QJsonObject &root)
{
    m_trackInfoDialogState = root.value(QStringLiteral("trackInfoDialog")).toObject();
    applyTrackInfoSettingsJson(root);
}

void TrackInfoPanel::resetToDefaults()
{
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
    auto *paneLayout = qobject_cast<QVBoxLayout *>(layout());
    if (paneLayout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        paneLayout->removeWidget(m_trackInfoBottomSpacer);
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
        if (paneLayout != nullptr) {
            paneLayout->removeWidget(label);
            paneLayout->addWidget(label);
        }
    }
    if (paneLayout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        paneLayout->addWidget(m_trackInfoBottomSpacer);
    }
    restyleTrackInfoLabels();
    applyTrackInfoLayoutSpacing();
    updateTrackInfoLabels();
}

void TrackInfoPanel::applyTrackInfoSettingsJson(const QJsonObject &root)
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
    auto *paneLayout = qobject_cast<QVBoxLayout *>(layout());
    if (paneLayout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        paneLayout->removeWidget(m_trackInfoBottomSpacer);
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
        if (paneLayout != nullptr) {
            paneLayout->removeWidget(label);
            paneLayout->addWidget(label);
        }
    }
    if (paneLayout != nullptr && m_trackInfoBottomSpacer != nullptr) {
        paneLayout->addWidget(m_trackInfoBottomSpacer);
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

QJsonArray TrackInfoPanel::trackInfoSettingsJson() const
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
    auto *paneLayout = qobject_cast<QVBoxLayout *>(layout());
    for (int index = 0; paneLayout != nullptr && index < paneLayout->count(); ++index) {
        QWidget *label = paneLayout->itemAt(index)->widget();
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

QJsonArray TrackInfoPanel::trackInfoMetadataSettingsJson() const
{
    return normalizedMetadataItems(m_trackInfoMetadataItems);
}

void TrackInfoPanel::applyTrackInfoLayoutSpacing()
{
    auto *paneLayout = qobject_cast<QVBoxLayout *>(layout());
    if (paneLayout == nullptr) {
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
        paneLayout->setSpacing(m_trackInfoLineSpacing);
        return;
    }
    paneLayout->setSpacing(1);
}

void TrackInfoPanel::restyleTrackInfoLabels()
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

QWidget *TrackInfoPanel::trackInfoLabelFromSender() const
{
    return qobject_cast<QWidget *>(sender());
}

void TrackInfoPanel::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        restyleTrackInfoLabels();
        updateTrackInfoLabels();
        update();
    }
}

void TrackInfoPanel::showTrackInfoLabelMenu(const QPoint &pos)
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
    QAction *configureAction = menu.addAction(QStringLiteral("Configure track information…"));
    const QAction *selected = menu.exec(label->mapToGlobal(pos));
    if (selected == copy) {
        QGuiApplication::clipboard()->setText(text);
    } else if (selected == configureAction) {
        configure(this);
    }
}
