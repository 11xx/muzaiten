#include "ui/trackinfo/TrackInfoPanel.h"

#include "ui/NeighborColumnResizer.h"
#include "ui/ReorderableTableWidget.h"
#include "ui/trackinfo/TrackInfoSettings.h"

#include <QAction>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
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
    QDialog dialog(dialogParent);
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
        emit settingsChanged();
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
    QAction *configureAction = menu.addAction(QStringLiteral("Configure track information..."));
    const QAction *selected = menu.exec(label->mapToGlobal(pos));
    if (selected == copy) {
        QGuiApplication::clipboard()->setText(text);
    } else if (selected == configureAction) {
        configure(this);
    }
}
