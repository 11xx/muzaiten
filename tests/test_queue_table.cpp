#include "ui/HeaderLabelStyle.h"
#include "ui/QueueStore.h"
#include "ui/QueueTable.h"
#include "ui/QueueKeybindings.h"
#include "ui/ResponsiveColumnLayout.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QBrush>
#include <QHeaderView>
#include <QLineEdit>
#include <QTableView>
#include <QTest>

class QueueTableTest : public QObject {
    Q_OBJECT

private slots:
    void defaultBindingsTreatCtrlGAsEscape()
    {
        const KeyBindingMap bindings = queueBindingMapForProfile(QStringLiteral("dired_hjkl"));

        QCOMPARE(bindings.value(QKeySequence(Qt::Key_Escape)), QString::fromLatin1(QueueAction::Escape));
        QCOMPARE(bindings.value(QKeySequence(Qt::ControlModifier | Qt::Key_G)), QString::fromLatin1(QueueAction::Escape));
    }

    void fullScreenDefaultsUseNcmpcppOrderWithRatingLast()
    {
        QueueTable table(QueueTablePreset::FullScreen);
        auto *view = table.findChild<QTableView *>();
        QVERIFY(view != nullptr);

        QVector<int> visible;
        for (int visual = 0; visual < view->horizontalHeader()->count(); ++visual) {
            const int logical = view->horizontalHeader()->logicalIndex(visual);
            if (!view->isColumnHidden(logical)) {
                visible.push_back(logical);
            }
        }
        QCOMPARE(visible, (QVector<int>{4, 8, 1, 5, 6, 2}));
    }

    void fullScreenDefaultsUseScreenshotProportions()
    {
        QueueTable table(QueueTablePreset::FullScreen);
        auto *view = table.findChild<QTableView *>();
        QVERIFY(view != nullptr);

        const QVector<int> visible{4, 8, 1, 5, 6, 2};
        QVector<int> widths;
        for (int column : visible) {
            widths.push_back(view->columnWidth(column));
        }

        // Screenshot-derived defaults: Artist medium, Track small, Title widest,
        // Album broad, Duration tiny, Rating last and slightly wider than Duration.
        QVERIFY(widths.at(2) > widths.at(3));
        QVERIFY(widths.at(3) > widths.at(0));
        QVERIFY(widths.at(0) > widths.at(4));
        QVERIFY(widths.at(5) > widths.at(4));
    }

    void headerLabelsUseMutedThemeRoles()
    {
        QueueTable table(QueueTablePreset::FullScreen);
        auto *view = table.findChild<QTableView *>();
        QVERIFY(view != nullptr);
        static constexpr HeaderViewStyle kHeaderStyle{
            HeaderLabelStyle{QFont::Normal, true, HeaderLabelTone::Muted, 0.20},
            false,
        };

        const QVariant fontValue = view->model()->headerData(1, Qt::Horizontal, Qt::FontRole);
        QVERIFY(fontValue.isValid());
        QCOMPARE(fontValue.value<QFont>().weight(), QFont::Normal);

        const QVariant brushValue = view->model()->headerData(1, Qt::Horizontal, Qt::ForegroundRole);
        QVERIFY(brushValue.isValid());
        QCOMPARE(brushValue.value<QBrush>().color(), headerLabelBrush(QApplication::palette(), kHeaderStyle.labels).color());
        QCOMPARE(view->horizontalHeader()->styleSheet(), headerViewStyleSheet(kHeaderStyle, view->horizontalHeader()));
    }

    void displayDataUsesQueueColumnSemantics()
    {
        QueueStore store;
        Track track;
        track.path = QStringLiteral("/music/Artist/Album/01-file-title.flac");
        track.filename = QStringLiteral("01-file-title.flac");
        track.artistName = QStringLiteral("Track Artist");
        track.albumArtistName = QStringLiteral("Album Artist");
        track.albumTitle = QStringLiteral("Album");
        track.durationMs = 185000;
        track.trackNumber = 7;
        track.effectiveRating0To100 = 80;
        store.setSnapshot({track}, 0, -1, -1);

        QueueTable table(QueueTablePreset::FullScreen);
        table.setQueueStore(&store);
        auto *view = table.findChild<QTableView *>();
        QVERIFY(view != nullptr);
        auto *model = view->model();
        QVERIFY(model != nullptr);

        QCOMPARE(model->index(0, 4).data().toString(), QStringLiteral("Album Artist"));
        QCOMPARE(model->index(0, 1).data().toString(), QStringLiteral("01-file-title"));
        QCOMPARE(model->index(0, 6).data().toString(), QStringLiteral("3:05"));
        QCOMPARE(model->index(0, 8).data().toString(), QStringLiteral("7"));

        track.albumArtistName.clear();
        track.title = QStringLiteral("Tagged Title");
        store.setSnapshot({track}, 0, -1, -1);
        QCOMPARE(model->index(0, 4).data().toString(), QStringLiteral("Track Artist"));
        QCOMPARE(model->index(0, 1).data().toString(), QStringLiteral("Tagged Title"));
    }

    void settingsRoundTripVisibleColumns()
    {
        QueueTable first(QueueTablePreset::FullScreen);
        auto *firstLayout = first.findChild<ResponsiveColumnLayout *>();
        QVERIFY(firstLayout != nullptr);
        QSet<QString> visible = firstLayout->userVisibleColumns();
        visible.insert(QStringLiteral("year"));
        firstLayout->setUserVisibleColumns(visible);
        const QString json = first.viewSettingsJson();

        QueueTable second(QueueTablePreset::FullScreen);
        second.applyViewSettingsJson(json);
        auto *secondView = second.findChild<QTableView *>();
        QVERIFY(secondView != nullptr);
        QVERIFY(!secondView->isColumnHidden(7));
        QVERIFY(!secondView->isColumnHidden(2));
    }

    void defaultResponsivePriorities()
    {
        QueueTable table(QueueTablePreset::FullScreen);
        auto *layout = table.findChild<ResponsiveColumnLayout *>();
        QVERIFY(layout != nullptr);

        QCOMPARE(layout->columnPriority(QStringLiteral("title")), ResponsiveColumnPriority::Keep);
        QCOMPARE(layout->columnPriority(QStringLiteral("duration")), ResponsiveColumnPriority::HideEarly);
        QCOMPARE(layout->columnPriority(QStringLiteral("track")), ResponsiveColumnPriority::HideEarly);
        QCOMPARE(layout->columnPriority(QStringLiteral("year")), ResponsiveColumnPriority::HideEarly);
        QCOMPARE(layout->columnPriority(QStringLiteral("position")), ResponsiveColumnPriority::HideEarly);
    }

    void currentPlayingDoesNotStealSelection()
    {
        QueueStore store;
        Track first;
        first.path = QStringLiteral("/a.flac");
        first.title = QStringLiteral("A");
        Track second;
        second.path = QStringLiteral("/b.flac");
        second.title = QStringLiteral("B");
        store.setSnapshot({first, second}, 0, -1, -1);

        QueueTable table(QueueTablePreset::FullScreen);
        table.setQueueStore(&store);
        auto *view = table.findChild<QTableView *>();
        QVERIFY(view != nullptr);
        view->setCurrentIndex(view->model()->index(1, 0));
        view->selectRow(1);

        store.setCurrentIndex(0);
        QCOMPARE(view->currentIndex().row(), 1);
    }

    void playNextOrdinalRoleUpdatesFromStore()
    {
        QueueStore store;
        Track first;
        first.path = QStringLiteral("/a.flac");
        Track second;
        second.path = QStringLiteral("/b.flac");
        Track third;
        third.path = QStringLiteral("/c.flac");
        store.setSnapshot({first, second, third}, 0, 1, 3);

        QueueTable table(QueueTablePreset::FullScreen);
        table.setQueueStore(&store);
        auto *view = table.findChild<QTableView *>();
        QVERIFY(view != nullptr);

        QCOMPARE(view->model()->index(1, 0).data(Qt::UserRole + 3).toInt(), 1);
        QCOMPARE(view->model()->index(2, 0).data(Qt::UserRole + 3).toInt(), 2);
        store.setPlayNextRange(-1, -1);
        QCOMPARE(view->model()->index(1, 0).data(Qt::UserRole + 3).toInt(), 0);
    }

    void slashSearchJumpsToFirstMatchingRow()
    {
        QueueStore store;
        Track a; a.path = QStringLiteral("/a.flac"); a.title = QStringLiteral("Alpha");
        Track b; b.path = QStringLiteral("/b.flac"); b.title = QStringLiteral("Beta");
        Track c; c.path = QStringLiteral("/c.flac"); c.title = QStringLiteral("Gamma");
        Track d; d.path = QStringLiteral("/d.flac"); d.title = QStringLiteral("Gamma II");
        store.setTracks({a, b, c, d});

        QueueTable table(QueueTablePreset::FullScreen);
        table.setQueueStore(&store);

        // The inline search bar exists for the full-screen queue.
        auto *edit = table.findChild<QLineEdit *>();
        QVERIFY(edit != nullptr);

        // Typing drives the same matcher the main panels use; two rows match
        // "gamma" and the cursor jumps to the first (row 2).
        edit->setText(QStringLiteral("gamma"));
        QCOMPARE(table.currentRow(), 2);

        edit->setText(QStringLiteral("beta"));
        QCOMPARE(table.currentRow(), 1);

        // A query with no matches leaves the cursor where it was.
        edit->setText(QStringLiteral("zzz-no-such-track"));
        QCOMPARE(table.currentRow(), 1);
    }

    void sidebarQueueHasNoInlineSearchBar()
    {
        // The sidebar queue gets its search from PanelSearchController instead,
        // so it must not build its own inline search bar.
        QueueTable table(QueueTablePreset::Sidebar);
        QVERIFY(table.findChild<QLineEdit *>() == nullptr);
    }
};

QTEST_MAIN(QueueTableTest)

#include "test_queue_table.moc"
