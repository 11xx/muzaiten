#include "ui/SearchView.h"

#include "core/Track.h"
#include "search/Exclusion.h"
#include "search/RankConfig.h"
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
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollBar>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

Q_DECLARE_METATYPE(QVector<Search::ScoredResult>)
Q_DECLARE_METATYPE(Search::SearchRecord)
Q_DECLARE_METATYPE(Search::ScoredResult)

namespace {
// Keep the index resident for this long after the search view is hidden, so
// re-opening search feels instant. After it elapses the index memory is freed
// (it's the bulk of the app's heap for a large library, so reclaim it promptly).
constexpr int kIndexCleanupMs = 60000; // 1 minute
}

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
    m_resultList->setContextMenuPolicy(Qt::CustomContextMenu);
    // The cursor is driven explicitly; suppress Qt's type-ahead keyboard search.
    m_resultList->setTabKeyNavigation(false);

    m_model    = new SearchResultsModel(this);
    m_delegate = new SearchResultDelegate(this);
    m_resultList->setModel(m_model);
    m_resultList->setItemDelegate(m_delegate);

    OverlayScrollBar::install(m_resultList);

    layout->addWidget(m_resultList, 1);

    // Debounce timer — short, just to coalesce rapid keystrokes.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(20);

    // Cleanup timer — frees the resident index a while after the view is hidden.
    m_cleanupTimer = new QTimer(this);
    m_cleanupTimer->setSingleShot(true);
    m_cleanupTimer->setInterval(kIndexCleanupMs);

    // Connections
    connect(m_searchBox, &QLineEdit::textChanged, this, &SearchView::onTextChanged);
    connect(m_debounce,  &QTimer::timeout,         this, &SearchView::onDebounceTimeout);
    connect(m_cleanupTimer, &QTimer::timeout,      this, &SearchView::onCleanupTimeout);

    connect(m_resultList, &QListView::doubleClicked, this, &SearchView::onDoubleClicked);
    connect(m_resultList, &QListView::customContextMenuRequested, this, &SearchView::showContextMenu);

    // Mirror the model's current index into the delegate so the cursor row is
    // always highlighted, even while the search box keeps keyboard focus.
    connect(m_resultList->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &cur, const QModelIndex &) {
                m_delegate->setCurrentRow(cur.isValid() ? cur.row() : -1);
                m_resultList->viewport()->update();
            });
    connect(m_resultList->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection &, const QItemSelection &) {
                m_resultList->viewport()->update();
            });

    // Intercept keys: the search box owns input, the list owns it after a click.
    m_searchBox->installEventFilter(this);
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

void SearchView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_cleanupTimer->stop();           // staying resident
    if (!m_dbPath.isEmpty()) {
        ensureIndexLoaded(m_dbPath);  // rebuild if it was freed while hidden
    }
}

void SearchView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // Keep the index for a buffer window, then free it.
    m_cleanupTimer->start();
}

bool SearchView::eventFilter(QObject *watched, QEvent *event)
{
    // Key handling on the search box (input mode) and the list (browse mode).
    if (watched == m_searchBox && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (handleNavKey(ke)) {
            return true;
        }
        // Everything else (printable text, editing keys) goes to the line edit.
        return false;
    }

    if (watched == m_resultList && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // In browse mode, '/' (or C-s) returns to the search box.
        if ((ke->key() == Qt::Key_Slash && ke->modifiers() == Qt::NoModifier)
            || (ke->key() == Qt::Key_S && ke->modifiers() == Qt::ControlModifier)) {
            focusSearchBox();
            return true;
        }
        if (handleNavKey(ke)) {
            return true;
        }
        // Typing a printable character jumps back to the box and forwards it.
        const QString text = ke->text();
        if (!text.isEmpty() && text[0].isPrint()) {
            focusSearchBox();
            m_searchBox->setText(m_searchBox->text() + text);
            return true;
        }
        return false;
    }

    // Wheel + hover on the list viewport.
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

// NOTE: the Keybinds-dialog reference for this view lives in
// ui/ViewKeyReferences.cpp — update it when changing keys here.
bool SearchView::handleNavKey(QKeyEvent *ke)
{
    const auto mods = ke->modifiers();
    const int key = ke->key();
    const bool ctrl = mods & Qt::ControlModifier;
    const int pageStep = std::max(1, m_resultList->height() / std::max(1, m_delegate->rowHeight() > 0
                                                                        ? m_delegate->rowHeight()
                                                                        : 64));

    // Force refresh
    if (key == Qt::Key_F5) { forceRefresh(); return true; }

    // Navigation (cursor moves without disturbing marks)
    if (key == Qt::Key_Down  || (ctrl && key == Qt::Key_N)) { moveCursor(+1); return true; }
    if (key == Qt::Key_Up    || (ctrl && key == Qt::Key_P)) { moveCursor(-1); return true; }
    if (key == Qt::Key_PageDown) { moveCursor(+pageStep); return true; }
    if (key == Qt::Key_PageUp)   { moveCursor(-pageStep); return true; }
    if (key == Qt::Key_Home && !ctrl) { moveCursor(-1000000000); return true; }
    if (key == Qt::Key_End  && !ctrl) { moveCursor(+1000000000); return true; }

    // Activate (Enter = queue, Alt+Enter = play now)
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        activateSelection(mods & Qt::AltModifier);
        return true;
    }

    // Multi-select marks
    if (key == Qt::Key_Tab) { toggleMarkAtCursor(); moveCursor(+1); return true; }
    if (ctrl && key == Qt::Key_Space) { toggleMarkAtCursor(); return true; }
    if (ctrl && key == Qt::Key_A) { m_resultList->selectAll(); return true; }
    // Browse mode only: while the search box has focus, 'a' must type normally.
    if (mods == Qt::NoModifier && key == Qt::Key_A && m_resultList->hasFocus()) {
        const QVector<Track> tracks = selectedTracks();
        if (!tracks.isEmpty()) { emit addToPlaylistRequested(tracks); }
        return true;
    }

    // Fuzzy toggle
    if (ctrl && key == Qt::Key_F) { toggleFuzzyMode(); return true; }

    // Escape / Ctrl+G: two-stage — clear text, then release input focus.
    if (key == Qt::Key_Escape || (ctrl && key == Qt::Key_G)) { escapeOrClear(); return true; }

    return false;
}

void SearchView::focusSearchBox()
{
    m_searchBox->setFocus();
    m_searchBox->deselect();
    m_searchBox->setCursorPosition(static_cast<int>(m_searchBox->text().length()));
}

void SearchView::releaseInputFocus()
{
    // Move focus to the list so window shortcuts (1/2/3) work, and '/' / Ctrl+P/N
    // navigation stays available.
    m_resultList->setFocus();
}

void SearchView::escapeOrClear()
{
    if (!m_searchBox->text().isEmpty()) {
        m_searchBox->clear();          // first press: clear the query
    } else {
        releaseInputFocus();           // second press: leave input mode
    }
}

void SearchView::ensureIndexLoaded(const QString &dbPath)
{
    m_dbPath = dbPath;
    setupWorker(dbPath);
    if (m_indexLoaded || m_buildPending) return;
    m_buildPending = true;
    QMetaObject::invokeMethod(m_worker, "buildIndex", Qt::QueuedConnection);
}

void SearchView::invalidateIndex(const QString &dbPath)
{
    m_dbPath = dbPath;
    if (!m_worker) return;
    m_buildPending = true;
    QMetaObject::invokeMethod(m_worker, "buildIndex", Qt::QueuedConnection);
}

void SearchView::forceRefresh()
{
    if (m_dbPath.isEmpty()) return;
    m_statusLabel->setText(QStringLiteral("Refreshing…"));
    invalidateIndex(m_dbPath);
}

void SearchView::setRankConfig(const Search::RankConfig &config)
{
    m_rankConfig = config;
    m_ranker.setConfig(config);
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "setExclusions", Qt::QueuedConnection,
                                  Q_ARG(QVector<Search::ExcludeRule>, m_rankConfig.excludes));
    }
    submitQuery(); // re-run with the new ranking + exclusions
}

void SearchView::setupWorker(const QString &dbPath)
{
    if (m_worker) return;
    m_dbPath = dbPath;
    qRegisterMetaType<QVector<Search::ExcludeRule>>();
    m_workerThread = new QThread(this);
    m_worker = new Search::SearchWorker(dbPath);
    m_worker->moveToThread(m_workerThread);
    // Push any exclusions already configured before the worker existed.
    if (!m_rankConfig.excludes.isEmpty()) {
        QMetaObject::invokeMethod(m_worker, "setExclusions", Qt::QueuedConnection,
                                  Q_ARG(QVector<Search::ExcludeRule>, m_rankConfig.excludes));
    }

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

void SearchView::onCleanupTimeout()
{
    if (isVisible()) return;  // became visible again
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "clearIndex", Qt::QueuedConnection);
    }
    m_indexLoaded = false;
    m_buildPending = false;
    m_totalIndexed = 0;
    m_model->clear();
}

void SearchView::onTextChanged()
{
    m_debounce->start(); // restart the debounce
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
        m_matchCount = 0;
        m_delegate->setQuery(Search::SearchQuery{}, m_fuzzyMode);
        m_model->clear();
        updateStatusLabel();
        return;
    }
    // Give the delegate the parsed query so it can highlight on-screen rows.
    m_delegate->setQuery(Search::SearchQuery::parse(text), m_fuzzyMode);
    ++m_queryId;
    QMetaObject::invokeMethod(m_worker, "runQuery", Qt::QueuedConnection,
                              Q_ARG(quint64, m_queryId),
                              Q_ARG(QString, text),
                              Q_ARG(bool, m_fuzzyMode));
}

void SearchView::onIndexReady(int count)
{
    m_indexLoaded = true;
    m_buildPending = false;
    m_totalIndexed = count;
    updateStatusLabel();
    submitQuery(); // re-run the current query against the new index
}

void SearchView::onIndexError(const QString &error)
{
    m_buildPending = false;
    m_statusLabel->setText(QStringLiteral("Index error: %1").arg(error));
}

void SearchView::onResultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int totalMatches)
{
    if (queryId != m_queryId) return; // stale result from a superseded query

    m_matchCount = totalMatches;
    // Front-end ranking: re-order the matched set by the user's criteria before
    // handing it to the model. The engine's relevance order is the input.
    m_ranker.sort(results);
    // Each ScoredResult embeds a copy of its SearchRecord, so the model/delegate
    // work entirely with data already copied across the thread boundary.
    m_model->setResults(std::move(results));

    // Always place the cursor on the first result (no mark) so keyboard
    // navigation works immediately, like a text cursor.
    if (m_model->rowCount() > 0) {
        m_resultList->scrollToTop();
        setCursorRow(0, /*select=*/false);
    } else {
        m_delegate->setCurrentRow(-1);
    }
    updateStatusLabel();
}

void SearchView::updateStatusLabel()
{
    if (!m_indexLoaded) {
        m_statusLabel->setText(QStringLiteral("Loading index…"));
        return;
    }
    const QString modeStr = m_fuzzyMode ? QStringLiteral("fuzzy") : QStringLiteral("exact");
    if (m_searchBox->text().isEmpty()) {
        m_statusLabel->setText(QStringLiteral("%1 tracks  ·  %2").arg(m_totalIndexed).arg(modeStr));
    } else {
        const int shown = m_model->rowCount();
        const QString countStr = (m_matchCount > shown)
            ? QStringLiteral("%1 of %2").arg(shown).arg(m_matchCount)  // capped display
            : QString::number(m_matchCount);
        m_statusLabel->setText(QStringLiteral("%1 / %2  ·  %3")
                                    .arg(countStr).arg(m_totalIndexed).arg(modeStr));
    }
}

void SearchView::moveCursor(int delta)
{
    const int rows = m_model->rowCount();
    if (rows == 0) return;
    const QModelIndex cur = m_resultList->currentIndex();
    const int row = cur.isValid() ? cur.row() : 0;
    setCursorRow(std::clamp(row + delta, 0, rows - 1), /*select=*/false);
}

void SearchView::setCursorRow(int row, bool select)
{
    const QModelIndex idx = m_model->index(row, 0);
    if (!idx.isValid()) return;
    auto *sm = m_resultList->selectionModel();
    sm->setCurrentIndex(idx, select ? QItemSelectionModel::ClearAndSelect
                                     : QItemSelectionModel::NoUpdate);
    m_resultList->scrollTo(idx, QAbstractItemView::EnsureVisible);
}

void SearchView::toggleMarkAtCursor()
{
    const QModelIndex cur = m_resultList->currentIndex();
    if (!cur.isValid()) return;
    m_resultList->selectionModel()->select(cur, QItemSelectionModel::Toggle);
}

Track SearchView::trackAt(const QModelIndex &index) const
{
    if (!index.isValid()) return {};
    const auto recVar = index.data(SearchResultsModel::SearchRecordRole);
    if (!recVar.isValid()) return {};
    return trackFromRecord(qvariant_cast<Search::SearchRecord>(recVar));
}

QVector<Track> SearchView::selectedTracks() const
{
    const QModelIndexList sel = m_resultList->selectionModel()->selectedIndexes();
    const QModelIndexList indices = sel.isEmpty()
        ? QModelIndexList{m_resultList->currentIndex()}
        : sel;

    QVector<Track> tracks;
    tracks.reserve(indices.size());
    for (const QModelIndex &idx : indices) {
        const Track t = trackAt(idx);
        if (!t.path.isEmpty()) tracks.append(t);
    }
    return tracks;
}

QVector<Track> SearchView::tracksForAction(const QModelIndex &clicked) const
{
    auto *sm = m_resultList->selectionModel();
    if (clicked.isValid() && sm->isSelected(clicked)) {
        return selectedTracks();  // act on the whole multi-selection
    }
    if (clicked.isValid()) {
        const Track t = trackAt(clicked);
        return t.path.isEmpty() ? QVector<Track>{} : QVector<Track>{t};
    }
    return selectedTracks();
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

void SearchView::onDoubleClicked(const QModelIndex &index)
{
    const QVector<Track> tracks = tracksForAction(index);
    if (!tracks.isEmpty()) {
        emit playNowRequested(tracks);
    }
}

void SearchView::showContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_resultList->indexAt(pos);
    if (!idx.isValid()) return;

    const Track single = trackAt(idx);
    const QVector<Track> targets = tracksForAction(idx);
    if (targets.isEmpty()) return;

    QMenu menu(this);
    const QString many = targets.size() > 1
        ? QStringLiteral(" (%1)").arg(targets.size()) : QString();

    menu.addAction(QStringLiteral("Play now%1").arg(many), this, [this, targets]() {
        emit playNowRequested(targets);
    });
    menu.addAction(QStringLiteral("Add to queue%1").arg(many), this, [this, targets]() {
        emit addToQueueRequested(targets);
    });
    menu.addAction(QStringLiteral("Add to playlist…%1").arg(many), this, [this, targets]() {
        emit addToPlaylistRequested(targets);
    });
    menu.addAction(QStringLiteral("Play next%1").arg(many), this, [this, targets]() {
        emit playNextRequested(targets);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Find in library"), this, [this, single]() {
        emit findInLibraryRequested(single);
    });
    menu.addAction(QStringLiteral("Open containing directory"), this, [this, single]() {
        emit findFileRequested(single);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Properties"), this, [this, single]() {
        emit propertiesRequested(single);
    });

    menu.exec(m_resultList->viewport()->mapToGlobal(pos));
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
    const int minH  = lineH * 4 + 6;
    const int maxH  = lineH * 4 + 40;

    if (m_rowHeight == 0) {
        m_rowHeight = m_delegate->sizeHint(QStyleOptionViewItem{}, QModelIndex{}).height();
    }
    m_rowHeight = std::clamp(m_rowHeight + delta, minH, maxH);
    m_delegate->setRowHeight(m_rowHeight);
    // Re-layout so the new row height takes effect.
    m_resultList->doItemsLayout();
    m_resultList->viewport()->update();
}
