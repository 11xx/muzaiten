#include "ui/trackinfo/TrackInfoSettingsDialog.h"

#include "ui/NeighborColumnResizer.h"
#include "ui/ReorderableTableWidget.h"
#include "ui/SettingsDialogSupport.h"
#include "ui/trackinfo/TrackInfoSettings.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace trackinfo {

TrackInfoDialogResult runTrackInfoSettingsDialog(QWidget *parent, const QJsonObject &current)
{
    const QJsonObject savedDialogState = current.value(QStringLiteral("trackInfoDialog")).toObject();

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Track Information"));
    dialog.resize(savedDialogState.value(QStringLiteral("width")).toInt(680),
                  savedDialogState.value(QStringLiteral("height")).toInt(480));
    auto *layout = new QVBoxLayout(&dialog);

    // Panel-wide controls first: alignment, line spacing and overflow govern the
    // whole information panel rather than any single field, so they sit above the
    // per-field and per-metadata tables.
    auto *panelOptionRow = new QHBoxLayout;
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Alignment"), &dialog));
    auto *alignment = new QComboBox(&dialog);
    alignment->addItem(QStringLiteral("Left"), QStringLiteral("left"));
    alignment->addItem(QStringLiteral("Center"), QStringLiteral("center"));
    alignment->addItem(QStringLiteral("Right"), QStringLiteral("right"));
    alignment->setCurrentIndex(std::max(0, alignment->findData(current.value(QStringLiteral("trackInfoAlignment")).toString(QStringLiteral("left")))));
    panelOptionRow->addWidget(alignment);
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Line spacing"), &dialog));
    auto *lineSpacingMode = new QComboBox(&dialog);
    lineSpacingMode->addItem(QStringLiteral("Justify"), QStringLiteral("justify"));
    lineSpacingMode->addItem(QStringLiteral("Fixed"), QStringLiteral("fixed"));
    lineSpacingMode->setCurrentIndex(std::max(0, lineSpacingMode->findData(current.value(QStringLiteral("trackInfoLineSpacingMode")).toString(QStringLiteral("justify")))));
    panelOptionRow->addWidget(lineSpacingMode);
    auto *lineSpacing = new QSpinBox(&dialog);
    lineSpacing->setRange(0, 16);
    lineSpacing->setValue(current.value(QStringLiteral("trackInfoLineSpacing")).toInt(1));
    lineSpacing->setSuffix(QStringLiteral(" px"));
    lineSpacing->setEnabled(lineSpacingMode->currentData().toString() == QStringLiteral("fixed"));
    panelOptionRow->addWidget(lineSpacing);
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Vertical"), &dialog));
    auto *verticalAlignment = new QComboBox(&dialog);
    verticalAlignment->addItem(QStringLiteral("Top"), QStringLiteral("top"));
    verticalAlignment->addItem(QStringLiteral("Center"), QStringLiteral("center"));
    verticalAlignment->addItem(QStringLiteral("Bottom"), QStringLiteral("bottom"));
    verticalAlignment->setCurrentIndex(std::max(0, verticalAlignment->findData(current.value(QStringLiteral("trackInfoVerticalAlignment")).toString(QStringLiteral("top")))));
    verticalAlignment->setEnabled(lineSpacingMode->currentData().toString() == QStringLiteral("fixed"));
    panelOptionRow->addWidget(verticalAlignment);
    panelOptionRow->addWidget(new QLabel(QStringLiteral("Overflow"), &dialog));
    auto *overflowMode = new QComboBox(&dialog);
    overflowMode->addItem(QStringLiteral("Scroll"));
    overflowMode->addItem(QStringLiteral("Truncate"));
    overflowMode->setCurrentIndex(current.value(QStringLiteral("trackInfoOverflowMode")).toString() == QStringLiteral("truncate") ? 1 : 0);
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

    for (const QJsonValue &value : current.value(QStringLiteral("trackInfoFields")).toArray()) {
        appendFieldRow(value.toObject());
    }
    settingsdialog::applyColumnWidths(table, {54, 240, 96, 76},
                                      savedDialogState.value(QStringLiteral("fieldCols")).toArray());

    auto *buttonRow = new QHBoxLayout;
    auto *up = new QPushButton(QStringLiteral("Up"), &dialog);
    auto *down = new QPushButton(QStringLiteral("Down"), &dialog);
    buttonRow->addWidget(up);
    buttonRow->addWidget(down);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);
    table->reorder = reorderFields;
    QObject::connect(up, &QPushButton::clicked, &dialog, [table, reorderFields]() {
        const int row = table->currentRow();
        if (row >= 0) {
            reorderFields(row, row - 1);
        }
    });
    QObject::connect(down, &QPushButton::clicked, &dialog, [table, reorderFields]() {
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
    const QString currentSeparator = current.value(QStringLiteral("trackInfoMetadataSeparator")).toString(QString::fromUtf8("\xc2\xb7"));
    separatorPreset->setCurrentText(separatorPresetLabel(currentSeparator));
    auto *separatorCustom = new QLineEdit(&dialog);
    separatorCustom->setMaximumWidth(80);
    separatorCustom->setText(currentSeparator);
    separatorCustom->setEnabled(separatorPreset->currentText() == QStringLiteral("Custom"));
    metadataOptionRow->addWidget(separatorPreset);
    metadataOptionRow->addWidget(separatorCustom);
    metadataOptionRow->addSpacing(12);
    metadataOptionRow->addWidget(new QLabel(QStringLiteral("Item spacing"), &dialog));
    auto *metadataSpacing = new QSpinBox(&dialog);
    metadataSpacing->setRange(0, 6);
    metadataSpacing->setValue(current.value(QStringLiteral("trackInfoMetadataSpacing")).toInt(1));
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

    for (const QJsonValue &value : normalizedMetadataItems(current.value(QStringLiteral("trackInfoMetadataItems")).toArray())) {
        appendMetadataRow(value.toObject());
    }
    settingsdialog::applyColumnWidths(metadataTable, {54, 220, 190},
                                      savedDialogState.value(QStringLiteral("metaCols")).toArray());

    QObject::connect(metadataTable, &QTableWidget::itemSelectionChanged, &dialog, [refreshNotable]() { refreshNotable(); });
    QObject::connect(notableSpin, &QSpinBox::valueChanged, &dialog, [metadataTable](int value) {
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
    QObject::connect(metadataUp, &QPushButton::clicked, &dialog, [metadataTable, reorderMetadata]() {
        const int row = metadataTable->currentRow();
        if (row >= 0) {
            reorderMetadata(row, row - 1);
        }
    });
    QObject::connect(metadataDown, &QPushButton::clicked, &dialog, [metadataTable, reorderMetadata]() {
        const int row = metadataTable->currentRow();
        if (row >= 0) {
            reorderMetadata(row, row + 2);
        }
    });

    QObject::connect(separatorPreset, &QComboBox::currentTextChanged, &dialog, [separatorPreset, separatorCustom]() {
        const bool custom = separatorPreset->currentText() == QStringLiteral("Custom");
        separatorCustom->setEnabled(custom);
        if (!custom) {
            separatorCustom->setText(separatorPreset->currentData().toString());
        }
    });
    QObject::connect(lineSpacingMode, &QComboBox::currentIndexChanged, &dialog, [lineSpacingMode, lineSpacing, verticalAlignment]() {
        const bool fixed = lineSpacingMode->currentData().toString() == QStringLiteral("fixed");
        lineSpacing->setEnabled(fixed);
        verticalAlignment->setEnabled(fixed);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    TrackInfoDialogResult result;
    result.accepted = dialog.exec() == QDialog::Accepted;

    // Remember the dialog's own UI state (size, column widths) regardless of the
    // outcome, so reopening keeps the layout the user arranged.
    result.dialogState.insert(QStringLiteral("width"), dialog.width());
    result.dialogState.insert(QStringLiteral("height"), dialog.height());
    result.dialogState.insert(QStringLiteral("fieldCols"), settingsdialog::columnWidths(table));
    result.dialogState.insert(QStringLiteral("metaCols"), settingsdialog::columnWidths(metadataTable));

    if (!result.accepted) {
        return result;
    }

    QJsonObject root;
    QJsonArray fields;
    for (int row = 0; row < table->rowCount(); ++row) {
        fields.append(readFieldRow(row));
    }
    root.insert(QStringLiteral("trackInfoFields"), fields);
    QJsonArray metadataItems;
    for (int row = 0; row < metadataTable->rowCount(); ++row) {
        metadataItems.append(readMetadataRow(row));
    }
    root.insert(QStringLiteral("trackInfoMetadataItems"), metadataItems);
    root.insert(QStringLiteral("trackInfoMetadataSeparator"), separatorCustom->text());
    root.insert(QStringLiteral("trackInfoMetadataSpacing"), metadataSpacing->value());
    root.insert(QStringLiteral("trackInfoAlignment"), alignment->currentData().toString());
    root.insert(QStringLiteral("trackInfoLineSpacingMode"), lineSpacingMode->currentData().toString());
    root.insert(QStringLiteral("trackInfoLineSpacing"), lineSpacing->value());
    root.insert(QStringLiteral("trackInfoVerticalAlignment"), verticalAlignment->currentData().toString());
    root.insert(QStringLiteral("trackInfoOverflowMode"), overflowMode->currentIndex() == 1 ? QStringLiteral("truncate") : QStringLiteral("scroll"));
    result.settings = root;
    return result;
}

} // namespace trackinfo
