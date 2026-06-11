#include "ui/PlaylistImportDialog.h"

#include <QColor>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

namespace {

QString decisionLabel(PlaylistMatcher::Decision decision)
{
    switch (decision) {
    case PlaylistMatcher::Decision::Matched:    return QStringLiteral("matched");
    case PlaylistMatcher::Decision::MultiMatch: return QStringLiteral("multi");
    case PlaylistMatcher::Decision::Pending:    return QStringLiteral("pending");
    }
    return QString();
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} // namespace

PlaylistImportDialog::PlaylistImportDialog(const QString &dbPath, const QString &playlistName,
                                           QWidget *parent)
    : QDialog(parent)
    , m_dbPath(dbPath)
{
    setWindowTitle(QStringLiteral("Import into \"%1\"").arg(playlistName));
    setModal(true);
    resize(720, 620);

    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(QStringLiteral(
        "Paste a tracklist (one \"Artist - Title\" per line), an m3u/m3u8, or csv — "
        "or load a file. Nothing is written until you press Add."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(QStringLiteral("Miles Davis - So What\nRadiohead - Karma Police\n…"));
    layout->addWidget(m_input, 2);

    auto *buttonRow = new QHBoxLayout();
    auto *loadButton = new QPushButton(QStringLiteral("Load file…"), this);
    connect(loadButton, &QPushButton::clicked, this, &PlaylistImportDialog::loadFile);
    buttonRow->addWidget(loadButton);
    m_matchButton = new QPushButton(QStringLiteral("Match against library"), this);
    m_matchButton->setDefault(true);
    connect(m_matchButton, &QPushButton::clicked, this, &PlaylistImportDialog::runMatch);
    buttonRow->addWidget(m_matchButton);
    m_status = new QLabel(this);
    buttonRow->addWidget(m_status, 1);
    layout->addLayout(buttonRow);

    m_preview = new QTableWidget(0, 4, this);
    m_preview->setHorizontalHeaderLabels({QStringLiteral("Status"), QStringLiteral("Title"),
                                          QStringLiteral("Artist"), QStringLiteral("Resolved to")});
    m_preview->horizontalHeader()->setStretchLastSection(true);
    m_preview->verticalHeader()->setVisible(false);
    m_preview->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_preview, 3);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_addButton = buttons->button(QDialogButtonBox::Ok);
    m_addButton->setText(QStringLiteral("Add"));
    m_addButton->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Edits invalidate a finished match until re-run.
    connect(m_input, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_results.isEmpty()) {
            m_results.clear();
            m_preview->setRowCount(0);
            m_addButton->setEnabled(false);
            m_status->setText(QStringLiteral("Edited — match again."));
        }
    });
}

PlaylistImportDialog::~PlaylistImportDialog()
{
    if (m_workerThread != nullptr) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
        m_workerThread = nullptr;
        m_worker = nullptr;  // deleteLater on thread finish
    }
}

void PlaylistImportDialog::ensureWorker()
{
    if (m_worker != nullptr) {
        return;
    }
    qRegisterMetaType<QVector<PlaylistImport::ImportEntry>>();
    qRegisterMetaType<QVector<PlaylistImportMatch>>();
    m_workerThread = new QThread(this);
    m_worker = new PlaylistImportWorker(m_dbPath);
    m_worker->moveToThread(m_workerThread);
    connect(m_worker, &PlaylistImportWorker::progress, this, &PlaylistImportDialog::onProgress,
            Qt::QueuedConnection);
    connect(m_worker, &PlaylistImportWorker::finished, this, &PlaylistImportDialog::onFinished,
            Qt::QueuedConnection);
    connect(m_worker, &PlaylistImportWorker::error, this, &PlaylistImportDialog::onError,
            Qt::QueuedConnection);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread->start();
}

void PlaylistImportDialog::loadFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import playlist file"), QString(),
        QStringLiteral("Playlists (*.m3u *.m3u8 *.csv *.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    const auto entries = PlaylistImport::parseFile(path, &error);
    if (entries.isEmpty()) {
        m_status->setText(error.isEmpty() ? QStringLiteral("No entries found in the file.")
                                          : QStringLiteral("Load failed: %1").arg(error));
        return;
    }
    // Re-render the file as text in the input box so the user can prune lines
    // before matching; m3u/csv structure is preserved by keeping the raw file.
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_input->setPlainText(QString::fromUtf8(file.readAll()));
    }
    m_status->setText(QStringLiteral("%1 entries loaded — press Match.").arg(entries.size()));
}

void PlaylistImportDialog::runMatch()
{
    if (m_matching) {
        return;
    }
    const auto entries = PlaylistImport::parse(m_input->toPlainText());
    if (entries.isEmpty()) {
        m_status->setText(QStringLiteral("Nothing to match."));
        return;
    }
    ensureWorker();
    m_matching = true;
    m_matchButton->setEnabled(false);
    m_addButton->setEnabled(false);
    m_status->setText(QStringLiteral("Building index…"));
    QMetaObject::invokeMethod(m_worker, "matchEntries", Qt::QueuedConnection,
                              Q_ARG(QVector<PlaylistImport::ImportEntry>, entries));
}

void PlaylistImportDialog::onProgress(int done, int total)
{
    m_status->setText(QStringLiteral("Matching… %1 / %2").arg(done).arg(total));
}

void PlaylistImportDialog::onFinished(QVector<PlaylistImportMatch> results)
{
    m_matching = false;
    m_matchButton->setEnabled(true);
    m_results = std::move(results);
    rebuildPreview();
    updateSummary();
    m_addButton->setEnabled(!m_results.isEmpty());
}

void PlaylistImportDialog::onError(const QString &message)
{
    m_matching = false;
    m_matchButton->setEnabled(true);
    m_status->setText(QStringLiteral("Import error: %1").arg(message));
}

void PlaylistImportDialog::rebuildPreview()
{
    m_preview->setRowCount(0);
    for (const PlaylistImportMatch &match : m_results) {
        const int row = m_preview->rowCount();
        m_preview->insertRow(row);

        auto *statusItem = readOnlyItem(decisionLabel(match.outcome.decision));
        QString resolved;
        QString title = match.entry.title;
        QString artist = match.entry.artist;
        switch (match.outcome.decision) {
        case PlaylistMatcher::Decision::Matched:
            title = match.outcome.best.title;
            artist = match.outcome.best.artistName;
            resolved = match.outcome.best.path;
            statusItem->setForeground(QColor(0x4c, 0xaf, 0x50));
            break;
        case PlaylistMatcher::Decision::MultiMatch:
            resolved = QStringLiteral("%1 candidates").arg(match.outcome.candidatePaths.size());
            statusItem->setForeground(QColor(0xff, 0xb7, 0x4d));
            break;
        case PlaylistMatcher::Decision::Pending:
            resolved = QStringLiteral("—");
            break;
        }
        m_preview->setItem(row, 0, statusItem);
        m_preview->setItem(row, 1, readOnlyItem(title));
        m_preview->setItem(row, 2, readOnlyItem(artist));
        m_preview->setItem(row, 3, readOnlyItem(resolved));
    }
    m_preview->resizeColumnToContents(0);
}

void PlaylistImportDialog::updateSummary()
{
    int matched = 0, multi = 0, pending = 0;
    for (const PlaylistImportMatch &match : m_results) {
        switch (match.outcome.decision) {
        case PlaylistMatcher::Decision::Matched:    ++matched; break;
        case PlaylistMatcher::Decision::MultiMatch: ++multi;   break;
        case PlaylistMatcher::Decision::Pending:    ++pending; break;
        }
    }
    m_status->setText(QStringLiteral("%1 matched · %2 multi · %3 pending — Add inserts all "
                                     "(multi/pending stay editable via 'e').")
                          .arg(matched).arg(multi).arg(pending));
}
