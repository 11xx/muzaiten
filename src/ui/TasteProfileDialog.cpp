#include "ui/TasteProfileDialog.h"

#include "app/AppPaths.h"
#include "reco/TasteProfile.h"
#include "reco/TrackScorer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace {

void previewLayout(QDialog &dialog, const QByteArray &bytes, QVBoxLayout *layout)
{
    dialog.resize(650, 550);
    auto *description = new QLabel(TasteProfiles::contentsDescription(), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *contents = new QPlainTextEdit(QString::fromUtf8(bytes), &dialog);
    contents->setObjectName(QStringLiteral("tastePreviewContents"));
    contents->setReadOnly(true);
    layout->addWidget(contents);
}

void showError(QWidget *parent, const QString &error)
{
    QMessageBox::warning(parent, QStringLiteral("Taste profiles"), error);
}

// Both export variants from one reading of the library, so toggling decade
// shares in the preview never reads it again. Index 1 includes decades.
struct ExportPreview {
    QByteArray bytes[2];
    QString errors[2];
};

ExportPreview buildExportPreview(const QString &libraryPath, const QString &featuresPath)
{
    ExportPreview preview;
    QVector<TrackScorer::Candidate> tracks;
    QString error;
    if (!TasteProfiles::libraryCandidates(libraryPath, featuresPath, &tracks, &error)) {
        preview.errors[0] = preview.errors[1] = error;
        return preview;
    }
    for (int decades = 0; decades < 2; ++decades) {
        TasteProfile profile;
        if (!TasteProfiles::aggregate(tracks, true, decades == 1, &profile, &preview.errors[decades])
            || !TasteProfiles::encode(profile, &preview.bytes[decades], &preview.errors[decades])) {
            preview.bytes[decades].clear();
        }
    }
    return preview;
}

} // namespace

TasteProfileDialog::TasteProfileDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Taste profiles"));
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(QStringLiteral("Import an aggregate library profile as a named radio seed. "
        "Selecting a seed adds a bounded score bias; it does not establish a universal preference. "
        "Stop radio deactivates the seed. Files stay local."), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    m_seeds = new QComboBox(this);
    m_seeds->setObjectName(QStringLiteral("tasteSeeds"));
    layout->addWidget(m_seeds);
    auto *actions = new QHBoxLayout;
    auto *start = new QPushButton(QStringLiteral("Start radio"), this);
    auto *stop = new QPushButton(QStringLiteral("Stop radio"), this);
    auto *remove = new QPushButton(QStringLiteral("Remove seed"), this);
    actions->addWidget(start);
    actions->addWidget(stop);
    actions->addWidget(remove);
    layout->addLayout(actions);
    auto *exchange = new QHBoxLayout;
    auto *exportButton = new QPushButton(QStringLiteral("Preview library export…"), this);
    auto *importButton = new QPushButton(QStringLiteral("Preview import…"), this);
    exchange->addWidget(exportButton);
    exchange->addWidget(importButton);
    layout->addLayout(exchange);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(start, &QPushButton::clicked, this, [this]() {
        if (m_seeds->currentIndex() >= 0) {
            emit startRequested(m_seeds->currentText());
        }
    });
    connect(stop, &QPushButton::clicked, this, &TasteProfileDialog::stopRequested);
    connect(remove, &QPushButton::clicked, this, &TasteProfileDialog::removeProfile);
    connect(exportButton, &QPushButton::clicked, this, &TasteProfileDialog::exportProfile);
    connect(importButton, &QPushButton::clicked, this, &TasteProfileDialog::importProfile);
    reload();
}

void TasteProfileDialog::reload()
{
    const QString selected = m_seeds->currentText();
    QMap<QString, TasteProfile> profiles;
    QString error;
    m_seeds->clear();
    if (!TasteProfiles::load(&profiles, &error)) {
        showError(this, error);
        return;
    }
    m_seeds->addItems(profiles.keys());
    const int index = m_seeds->findText(selected);
    if (index >= 0) {
        m_seeds->setCurrentIndex(index);
    }
}

void TasteProfileDialog::exportProfile()
{
    QDialog preview(this);
    preview.setWindowTitle(QStringLiteral("Preview aggregate taste export"));
    auto *layout = new QVBoxLayout(&preview);
    auto *decades = new QCheckBox(QStringLiteral("Include supported decade shares"), &preview);
    decades->setEnabled(false);
    layout->addWidget(decades);
    previewLayout(preview, {}, layout);
    auto *contents = preview.findChild<QPlainTextEdit *>(QStringLiteral("tastePreviewContents"));
    contents->setPlainText(QStringLiteral("Reading the library…"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &preview);
    auto *write = buttons->addButton(QStringLiteral("Export these contents…"), QDialogButtonBox::AcceptRole);
    write->setEnabled(false);
    layout->addWidget(buttons);
    connect(write, &QPushButton::clicked, &preview, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);

    // The library is read off the GUI thread; Cancel closes the preview at once
    // and its result is dropped. What is shown is exactly what is written.
    ExportPreview built;
    QByteArray bytes;
    const auto show = [&]() {
        const int variant = decades->isChecked() ? 1 : 0;
        bytes = built.bytes[variant];
        contents->setPlainText(bytes.isEmpty() ? built.errors[variant] : QString::fromUtf8(bytes));
        write->setEnabled(!bytes.isEmpty());
    };
    QFutureWatcher<ExportPreview> reading;
    connect(&reading, &QFutureWatcherBase::finished, &preview, [&]() {
        built = reading.result();
        decades->setEnabled(true);
        show();
    });
    connect(decades, &QCheckBox::toggled, &preview, [&](bool) { show(); });
    const QDir dataDirectory(AppPaths::dataDir());
    reading.setFuture(QtConcurrent::run(buildExportPreview, dataDirectory.filePath(QStringLiteral("library.sqlite")),
                                        dataDirectory.filePath(QStringLiteral("features.sqlite"))));
    if (preview.exec() != QDialog::Accepted || bytes.isEmpty()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export taste profile"),
        QStringLiteral("taste-profile.json"), QStringLiteral("JSON files (*.json)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo target(path);
    const bool replace = target.exists() || target.isSymLink();
    QString error;
    if (replace && QMessageBox::question(this, QStringLiteral("Replace taste file"),
        QStringLiteral("Replace the existing file with the previewed contents?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!TasteProfiles::write(path, bytes, replace, &error)) {
        showError(this, error);
    }
}

void TasteProfileDialog::importProfile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import taste profile"), {},
                                                     QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    TasteProfile profile;
    QByteArray bytes;
    QString error;
    if (!TasteProfiles::read(path, &profile, &error) || !TasteProfiles::encode(profile, &bytes, &error)) {
        showError(this, error);
        return;
    }
    QDialog preview(this);
    preview.setWindowTitle(QStringLiteral("Preview taste import"));
    auto *layout = new QVBoxLayout(&preview);
    previewLayout(preview, bytes, layout);
    layout->addWidget(new QLabel(QStringLiteral("Local seed name (kept out of exchange files):"), &preview));
    auto *name = new QLineEdit(&preview);
    layout->addWidget(name);
    auto *replace = new QCheckBox(QStringLiteral("Replace an existing seed with this name"), &preview);
    layout->addWidget(replace);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &preview);
    auto *save = buttons->addButton(QStringLiteral("Import seed"), QDialogButtonBox::AcceptRole);
    save->setEnabled(false);
    connect(name, &QLineEdit::textChanged, save, [save](const QString &value) {
        save->setEnabled(TasteProfiles::validName(value));
    });
    connect(save, &QPushButton::clicked, &preview, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
    layout->addWidget(buttons);
    if (preview.exec() != QDialog::Accepted) {
        return;
    }
    QMap<QString, TasteProfile> seeds;
    if (!TasteProfiles::load(&seeds, &error)) {
        showError(this, error);
        return;
    }
    if (seeds.contains(name->text()) && !replace->isChecked()) {
        showError(this, QStringLiteral("Taste seed already exists. Select \"Replace an existing seed with this name\" in the import preview to replace it."));
        return;
    }
    if (!TasteProfiles::importProfile(name->text(), profile, replace->isChecked(), false, &error)) {
        showError(this, error);
        return;
    }
    reload();
    m_seeds->setCurrentText(name->text());
}

void TasteProfileDialog::removeProfile()
{
    if (m_seeds->currentIndex() < 0) {
        return;
    }
    const QString name = m_seeds->currentText();
    if (QMessageBox::question(this, QStringLiteral("Remove taste seed"),
        QStringLiteral("Remove the selected seed?"), QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!TasteProfiles::remove(name, &error)) {
        showError(this, error);
        return;
    }
    emit removed(name);
    reload();
}
