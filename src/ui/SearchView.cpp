#include "ui/SearchView.h"

#include "core/Track.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"
#include "search/SearchWorker.h"
#include "ui/OverlayScrollBar.h"
#include "ui/SearchResultDelegate.h"
#include "ui/SearchResultsModel.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPalette>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

Q_DECLARE_METATYPE(QVector<Search::ScoredResult>)
Q_DECLARE_METATYPE(Search::SearchRecord)
Q_DECLARE_METATYPE(Search::ScoredResult)

// Build a minimal Track from a SearchRecord — sufficient for playback and display.
static Track trackFromRecord(const Search::SearchRecord &rec)
{
    Track t;
    t.path             = rec.path;
    t.filename         = rec.filename;
    t.parentDir        = rec.path.contains(QLatin1Char('/'))
                          ? rec.path.left(rec.path.lastIndexOf(QLatin1Char('/')))
                          : QString();
    t.title            = rec.title;
    t.artistName       = rec.artistName;
    t.albumArtistName  = rec.albumArtistName;
    t.albumTitle       = rec.albumTitle;
    t.date             = rec.date;
    t.durationMs       = rec.durationMs;
    t.effectiveRating0To100 = rec.rating0To100;
    t.rating0To100     = rec.rating0To100;
    t.sampleRateHz     = rec.sampleRateHz;
    t.bitrateKbps      = rec.bitrateKbps;
    t.channels         = rec.channels;
    t.codec            = rec.codec;
    return t;
}

SearchView::SearchView(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

SearchView::~SearchView()
{
    teardownWorker();
}

void SearchView::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Search box row
    auto *topBar = new QFrame(this);
    topBar->setFrameShape(QFrame::NoFrame);
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(8, 6, 8, 6);
    topLayout->setSpacing(6);

    m_searchBox = new QLineEdit(topBar);
    m_searchBox->setPlaceholderText(QStringLiteral("Search library…  (artist:, album:, title:, ext:, khz:, kbps:, rating:, …)"));
    m_searchBox->setClearButtonEnabled(true);
    topLayout->addWidget(m_searchBox, 1);

    m_statusLabel = new QLabel(topBar);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    topLayout->addWidget(m_statusLabel);

    layout->addWidget(topBar);

    // Separator
    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    // Results list
    m_resultList = new QListView(this);
    m_resultList->setUniformItemSizes(false);
    m_resultList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_resultList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_resultList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_resultList->setFrameShape(QFrame::NoFrame);
    m_resultList->setFocusPolicy(Qt::StrongFocus);

    m_model    = new SearchResultsModel(this);
    m_delegate = new SearchResultDelegate(this);
    m_resultList->setModel(m_model);
    m_resultList->setItemDelegate(m_delegate);

    OverlayScrollBar::install(m_resultList);

    layout->addWidget(m_resultList, 1);

    // Debounce timer
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(60);

    // Connections
    connect(m_searchBox, &QLineEdit::textChanged, this, &SearchView::onTextChanged);
    connect(m_debounce,  &QTimer::timeout,         this, &SearchView::onDebounceTimeout);

    // Intercept key events on the result list so Tab and special keys work.
    m_resultList->installEventFilter(this);
    m_resultList->viewport()->installEventFilter(this);

    updateStatusLabel();
}

void SearchView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
     || event->type() == QEvent::ApplicationPaletteChange
     || event->type() == QEvent::StyleChange) {
        m_resultList->viewport()->update();
    }
}

bool SearchView::eventFilter(QObject *watched, QEvent *event)
{
    // Key events from the list view
    if (watched == m_resultList) {
        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            switch (ke->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                activateSelection(ke->modifiers() & Qt::AltModifier);
                return true;
            case Qt::Key_Tab:
                // Toggle selection on current item, move down
                {
                    const QModelIndex cur = m_resultList->currentIndex();
                    if (cur.isValid()) {
                        m_resultList->selectionModel()->select(
                            cur,
                            QItemSelectionModel::Toggle);
                        const int nextRow = cur.row() + 1;
                        if (nextRow < m_model->rowCount()) {
                            m_resultList->setCurrentIndex(m_model->index(nextRow));
                        }
                    }
                }
                return true;
            case Qt::Key_Space:
                if (ke->modifiers() & Qt::ControlModifier) {
                    // Ctrl+Space = toggle selection like Tab
                    const QModelIndex cur = m_resultList->currentIndex();
                    if (cur.isValid()) {
                        m_resultList->selectionModel()->select(
                            cur, QItemSelectionModel::Toggle);
                    }
                    return true;
                }
                break;
            case Qt::Key_A:
                if (ke->modifiers() & Qt::ControlModifier) {
                    m_resultList->selectAll();
                    return true;
                }
                break;
            case Qt::Key_F:
                if (ke->modifiers() & Qt::ControlModifier) {
                    toggleFuzzyMode();
                    return true;
                }
                break;
            case Qt::Key_Escape:
                emit leaveRequested();
                return true;
            case Qt::Key_P:
                if (ke->modifiers() & Qt::ControlModifier) {
                    // Ctrl+P = move up
                    const QModelIndex cur = m_resultList->currentIndex();
                    if (cur.isValid() && cur.row() > 0) {
                        m_resultList->setCurrentIndex(m_model->index(cur.row() - 1));
                    }
                    return true;
                }
                break;
            case Qt::Key_N:
                if (ke->modifiers() & Qt::ControlModifier) {
                    // Ctrl+N = move down
                    const QModelIndex cur = m_resultList->currentIndex();
                    if (cur.isValid()) {
                        const int next = cur.row() + 1;
                        if (next < m_model->rowCount()) {
                            m_resultList->setCurrentIndex(m_model->index(next));
                        }
                    }
                    return true;
                }
                break;
            default:
                break;
            }
        }
    }

    // Wheel events on the list viewport for Ctrl+scroll density
    if (watched == m_resultList->viewport()) {
        if (event->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(event);
            if (we->modifiers() & Qt::ControlModifier) {
                adjustRowHeight(we->angleDelta().y() > 0 ? 2 : -2);
                we->accept();
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove) {
            const auto *me = static_cast<QMouseEvent *>(event);
            const QModelIndex idx = m_resultList->indexAt(me->pos());
            const int row = idx.isValid() ? idx.row() : -1;
            if (row != m_delegate->hoveredRow()) {
                m_delegate->setHoveredRow(row);
                m_resultList->viewport()->update();
            }
        }
        if (event->type() == QEvent::Leave) {
            m_delegate->setHoveredRow(-1);
            m_resultList->viewport()->update();
        }
    }

    return QWidget::eventFilter(watched, event);
}

void SearchView::focusSearchBox()
{
    m_searchBox->setFocus();
    m_searchBox->selectAll();
}

void SearchView::ensureIndexLoaded(const QString &dbPath)
{
    if (m_indexLoaded && m_worker) return;
    setupWorker(dbPath);
    m_dbPath = dbPath;
    QMetaObject::invokeMethod(m_worker, "buildIndex", Qt::QueuedConnection);
}

void SearchView::invalidateIndex(const QString &dbPath)
{
    m_dbPath = dbPath;
    if (!m_worker) return;
    QMetaObject::invokeMethod(m_worker, "buildIndex", Qt::QueuedConnection);
}

void SearchView::setupWorker(const QString &dbPath)
{
    if (m_worker) return;
    m_dbPath = dbPath;
    m_workerThread = new QThread(this);
    m_worker = new Search::SearchWorker(dbPath);
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &Search::SearchWorker::indexReady,
            this, &SearchView::onIndexReady, Qt::QueuedConnection);
    connect(m_worker, &Search::SearchWorker::indexError,
            this, &SearchView::onIndexError, Qt::QueuedConnection);
    connect(m_worker, &Search::SearchWorker::resultsReady,
            this, &SearchView::onResultsReady, Qt::QueuedConnection);

    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread->start();
}

void SearchView::teardownWorker()
{
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
        m_workerThread = nullptr;
        m_worker = nullptr; // deleteLater handles it
    }
}

void SearchView::onTextChanged()
{
    m_debounce->start(); // restart the 60ms debounce
}

void SearchView::onDebounceTimeout()
{
    submitQuery();
}

void SearchView::submitQuery()
{
    if (!m_worker || !m_indexLoaded) return;
    const QString text = m_searchBox->text().trimmed();
    if (text.isEmpty()) {
        m_model->clear();
        updateStatusLabel();
        return;
    }
    ++m_queryId;
    QMetaObject::invokeMethod(m_worker, "runQuery", Qt::QueuedConnection,
                              Q_ARG(quint64, m_queryId),
                              Q_ARG(QString, text),
                              Q_ARG(bool, m_fuzzyMode));
}

void SearchView::onIndexReady(int count)
{
    m_indexLoaded = true;
    m_totalIndexed = count;
    updateStatusLabel();
    // Re-run the current query against the new index
    submitQuery();
}

void SearchView::onIndexError(const QString &error)
{
    m_statusLabel->setText(QStringLiteral("Index error: %1").arg(error));
}

void SearchView::onResultsReady(quint64 queryId, QVector<Search::ScoredResult> results)
{
    if (queryId != m_queryId) return; // stale result from a superseded query

    // Each ScoredResult carries an embedded copy of its SearchRecord, so there is
    // no cross-thread pointer sharing: the model and delegate work entirely with
    // the data in the ScoredResult vector, which was copied into the signal payload
    // when it crossed thread boundaries.
    m_model->setResults(std::move(results));
    updateStatusLabel();
}

void SearchView::updateStatusLabel()
{
    if (!m_indexLoaded) {
        m_statusLabel->setText(QStringLiteral("Loading index…"));
        return;
    }
    const int matchCount = m_model->rowCount();
    const QString modeStr = m_fuzzyMode ? QStringLiteral("fuzzy") : QStringLiteral("exact");
    if (m_searchBox->text().isEmpty()) {
        m_statusLabel->setText(QStringLiteral("%1 tracks  ·  %2")
                                    .arg(m_totalIndexed)
                                    .arg(modeStr));
    } else {
        m_statusLabel->setText(QStringLiteral("%1 / %2  ·  %3")
                                    .arg(matchCount)
                                    .arg(m_totalIndexed)
                                    .arg(modeStr));
    }
}

QVector<Track> SearchView::selectedTracks() const
{
    const QModelIndexList sel = m_resultList->selectionModel()->selectedIndexes();
    // If nothing selected, use the current (focused) item
    const QModelIndexList &indices = sel.isEmpty()
        ? QModelIndexList{m_resultList->currentIndex()}
        : sel;

    QVector<Track> tracks;
    tracks.reserve(indices.size());
    for (const QModelIndex &idx : indices) {
        if (!idx.isValid()) continue;
        const auto recVar = idx.data(SearchResultsModel::SearchRecordRole);
        if (!recVar.isValid()) continue;
        tracks.append(trackFromRecord(qvariant_cast<Search::SearchRecord>(recVar)));
    }
    return tracks;
}

void SearchView::activateSelection(bool playNow)
{
    const QVector<Track> tracks = selectedTracks();
    if (tracks.isEmpty()) return;
    if (playNow) {
        emit playNowRequested(tracks);
    } else {
        emit addToQueueRequested(tracks);
    }
}

void SearchView::toggleFuzzyMode()
{
    m_fuzzyMode = !m_fuzzyMode;
    updateStatusLabel();
    submitQuery();
}

void SearchView::adjustRowHeight(int delta)
{
    const QFontMetrics fm(font());
    const int lineH = fm.height();
    const int minH  = lineH * 4 + 6;  // at least 4 lines with minimal padding
    const int maxH  = lineH * 4 + 40;

    if (m_rowHeight == 0) {
        m_rowHeight = m_delegate->sizeHint(QStyleOptionViewItem{}, QModelIndex{}).height();
    }
    m_rowHeight = std::clamp(m_rowHeight + delta, minH, maxH);
    m_delegate->setRowHeight(m_rowHeight);
    // Force a re-layout by hiding and showing the viewport
    m_resultList->setUniformItemSizes(false);
    m_resultList->viewport()->update();
}
