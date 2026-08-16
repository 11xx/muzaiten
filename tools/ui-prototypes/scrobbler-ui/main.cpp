// Standalone prototype for two scrobbling UI surfaces, built and run on its own
// so the design can be iterated without rebuilding the application:
//
//   * the scrobbler manager as a per-destination grid, where every action is
//     reachable in one click and the fields edit in place;
//   * the listening history's destination filter as a multi-select popup that
//     stays open while destinations are toggled, next to a table that keeps its
//     selection visible while that popup holds focus.
//
// The widgets are stock Qt drawn by the active style, so a Kvantum or platform
// theme dresses this the same way it dresses the application. The one drawn
// control is the enable switch, which reads its colours out of the palette.
//
// It carries fake data and no persistence. Nothing here is linked into the
// application; see tools/ui-prototypes/README.md.

#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <functional>

namespace {

// The placeholder for a value a destination cannot have, matching the rest of
// the app's tables.
const auto kEmptyCell = QStringLiteral("—");

// ---------------------------------------------------------------- shared bits

enum class Health {
    Unknown,   // nothing tried yet
    Good,      // credentials verified
    Bad,       // credentials rejected, or a value the destination cannot work without
    Busy,      // a test is in flight
};

// Column headers, the empty-value placeholder, and other secondary text.
// PlaceholderText tracks the active theme, which a hardcoded colour cannot.
// The source is always the application palette, so tinting a widget twice does
// not compound.
void setMuted(QWidget *widget, bool muted)
{
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, QApplication::palette().color(muted ? QPalette::PlaceholderText
                                                                               : QPalette::WindowText));
    widget->setPalette(palette);
}

void makeMuted(QWidget *widget) { setMuted(widget, true); }

// A value, or the placeholder for its absence, which recedes rather than
// competing with the values around it.
void setValueOrDash(QLabel *label, const QString &value)
{
    label->setText(value.isEmpty() ? kEmptyCell : value);
    setMuted(label, value.isEmpty());
}

QFrame *horizontalRule(QWidget *parent)
{
    auto *rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    return rule;
}

// A dot ahead of the status word, sized to sit on the text's baseline.
class HealthDot final : public QWidget {
public:
    explicit HealthDot(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedWidth(10);
    }

    void setHealth(Health health)
    {
        m_health = health;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color());
        painter.drawEllipse(QPointF(width() / 2.0, height() / 2.0), 3.5, 3.5);
    }

private:
    QColor color() const
    {
        switch (m_health) {
        case Health::Good:
            return QColor(0x3f, 0xa0, 0x5f);
        case Health::Bad:
            return QColor(0xc6, 0x4b, 0x4b);
        case Health::Busy:
            return palette().color(QPalette::Highlight);
        case Health::Unknown:
            break;
        }
        return palette().color(QPalette::PlaceholderText);
    }

    Health m_health = Health::Unknown;
};

// A switch rather than a checkbox: enabling a destination is a live state that
// one click flips, and the switch says so without a label. Its colours come
// from the palette, so it follows the theme like everything around it.
class ToggleSwitch final : public QAbstractButton {
public:
    explicit ToggleSwitch(QWidget *parent = nullptr)
        : QAbstractButton(parent)
        , m_slide(new QVariantAnimation(this))
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        m_slide->setDuration(110);
        connect(m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_position = value.toReal();
            update();
        });
        connect(this, &QAbstractButton::toggled, this, [this](bool on) {
            m_slide->stop();
            m_slide->setStartValue(m_position);
            m_slide->setEndValue(on ? 1.0 : 0.0);
            m_slide->start();
        });
    }

    QSize sizeHint() const override { return {38, 20}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF track(1.5, (height() - 18) / 2.0, 33, 18);
        const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        const QColor off = palette().color(group, QPalette::Button);
        const QColor on = palette().color(group, QPalette::Highlight);
        const auto blend = [this](qreal from, qreal to) { return static_cast<float>(from + (to - from) * m_position); };
        // An off switch is a track with nothing in it, and on a theme where the
        // track and the window are near enough the same colour it would read as
        // nothing at all. The outline is what makes it a control either way.
        QColor outline = palette().color(group, QPalette::WindowText);
        outline.setAlphaF(0.45 - 0.35 * m_position);
        painter.setPen(QPen(outline, 1));
        painter.setBrush(QColor::fromRgbF(blend(off.redF(), on.redF()), blend(off.greenF(), on.greenF()),
                                          blend(off.blueF(), on.blueF())));
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);

        if (hasFocus()) {
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(track.adjusted(-1.5, -1.5, 1.5, 1.5), track.height() / 2 + 1,
                                    track.height() / 2 + 1);
        }

        const qreal travel = track.width() - track.height();
        const QPointF knob(track.left() + track.height() / 2 + travel * m_position, track.center().y());
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(group, QPalette::BrightText));
        painter.drawEllipse(knob, track.height() / 2 - 2.5, track.height() / 2 - 2.5);
    }

private:
    QVariantAnimation *m_slide = nullptr;
    qreal m_position = 0.0;
};

// ------------------------------------------------------- scrobbler manager tab

struct Destination {
    QString name;
    QString url;
    QString token;
    bool enabled = false;
    bool reserved = false;   // Last.fm and official ListenBrainz: identity and URL are not the user's
    bool lastFm = false;
    int pending = 0;
    Health health = Health::Unknown;
    QString status;
};

enum ManagerColumn {
    ToggleColumn,
    NameColumn,
    UrlColumn,
    TokenColumn,
    StatusColumn,
    PendingColumn,
    TestColumn,
    RemoveColumn,
    ManagerColumnCount,
};

// One destination's controls, laid into a grid shared by every row so the
// columns line up whatever each row happens to offer.
class DestinationRow final : public QObject {
public:
    DestinationRow(Destination destination, QGridLayout *grid, int row, QWidget *host,
                   std::function<void()> onChanged, std::function<void(DestinationRow *)> onRemove)
        : QObject(host)
        , m_destination(std::move(destination))
        , m_onChanged(std::move(onChanged))
        , m_onRemove(std::move(onRemove))
    {
        m_toggle = new ToggleSwitch(host);
        m_toggle->setChecked(m_destination.enabled);
        m_toggle->setToolTip(QStringLiteral("Send listens to this destination"));
        connect(m_toggle, &QAbstractButton::toggled, this, [this](bool on) {
            m_destination.enabled = on;
            refresh();
        });

        m_name = new QLineEdit(m_destination.name, host);
        m_name->setPlaceholderText(QStringLiteral("Koito"));
        m_url = new QLineEdit(m_destination.url, host);
        m_url->setPlaceholderText(QStringLiteral("https://koito.example"));
        m_token = new QLineEdit(m_destination.token, host);
        m_token->setEchoMode(QLineEdit::Password);
        m_token->setPlaceholderText(QStringLiteral("user token"));
        for (QLineEdit *field : {m_url, m_token}) {
            connect(field, &QLineEdit::textChanged, this, [this] {
                m_destination.status.clear();
                refresh();
            });
        }

        // A built-in destination is not the user's to rename or repoint, so it
        // shows the value as text rather than as a field that refuses input.
        m_fixedName = new QLabel(m_destination.name, host);
        m_fixedUrl = new QLabel(host);
        m_fixedUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setValueOrDash(m_fixedUrl, m_destination.lastFm ? QString() : m_destination.url);
        m_fixedToken = new QLabel(host);
        setValueOrDash(m_fixedToken, {});
        // Indented to the text inset of the fields beside them, so a fixed value
        // and an editable one start at the same x.
        const int inset = host->style()->pixelMetric(QStyle::PM_DefaultFrameWidth) + 3;
        for (QLabel *fixed : {m_fixedName, m_fixedUrl, m_fixedToken}) {
            fixed->setTextFormat(Qt::PlainText);
            fixed->setIndent(inset);
        }
        m_fixedToken->setToolTip(QStringLiteral("Last.fm signs in through Last.fm API settings"));

        m_dot = new HealthDot(host);
        m_status = new QLabel(host);
        m_pending = new QLabel(host);
        m_pending->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_test = new QPushButton(QStringLiteral("Test"), host);
        m_test->setToolTip(QStringLiteral("Check the server and credentials"));
        connect(m_test, &QPushButton::clicked, this, [this] {
            m_destination.health = Health::Busy;
            m_destination.status = QStringLiteral("Testing…");
            refresh();
            // Stand-in for the real network round trip.
            QTimer::singleShot(700, this, [this] {
                const bool ok = !m_token->text().isEmpty();
                m_destination.health = ok ? Health::Good : Health::Bad;
                m_destination.status = ok ? QStringLiteral("Connected as lobo") : QStringLiteral("Token rejected");
                refresh();
            });
        });

        m_remove = new QPushButton(host);
        m_remove->setIcon(QIcon::fromTheme(QStringLiteral("user-trash"),
                                           host->style()->standardIcon(QStyle::SP_TrashIcon)));
        m_remove->setToolTip(QStringLiteral("Remove this destination"));
        connect(m_remove, &QPushButton::clicked, this, [this] { m_onRemove(this); });

        const bool fixed = m_destination.reserved;
        auto *status = new QHBoxLayout;
        status->setContentsMargins(0, 0, 0, 0);
        status->setSpacing(4);
        status->addWidget(m_dot);
        status->addWidget(m_status);
        status->addStretch();

        grid->addWidget(m_toggle, row, ToggleColumn);
        grid->addWidget(fixed ? static_cast<QWidget *>(m_fixedName) : m_name, row, NameColumn);
        grid->addWidget(fixed ? static_cast<QWidget *>(m_fixedUrl) : m_url, row, UrlColumn);
        grid->addWidget(m_destination.lastFm ? static_cast<QWidget *>(m_fixedToken) : m_token, row, TokenColumn);
        grid->addLayout(status, row, StatusColumn);
        grid->addWidget(m_pending, row, PendingColumn);
        grid->addWidget(m_test, row, TestColumn);
        grid->addWidget(m_remove, row, RemoveColumn);

        for (QWidget *unused : unusedWidgets()) {
            unused->hide();
        }
        // A row that offers fewer actions still reserves their space, so the
        // action columns stay columns.
        m_test->setVisible(!m_destination.lastFm);
        m_remove->setVisible(!m_destination.reserved);
        for (QPushButton *button : {m_test, m_remove}) {
            QSizePolicy policy = button->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            button->setSizePolicy(policy);
        }

        refresh();
    }

    // A ListenBrainz-compatible destination with no address has nowhere to
    // deliver, which is fatal for that entry rather than a warning about it.
    bool isValid() const { return m_destination.lastFm || !currentUrl().isEmpty(); }
    QString name() const { return m_destination.reserved ? m_destination.name : m_name->text().trimmed(); }

    void takeOutOf(QGridLayout *grid)
    {
        const QList<QWidget *> owned{m_toggle,     m_name,   m_url,     m_token,   m_fixedName, m_fixedUrl,
                                     m_fixedToken, m_dot,    m_status,  m_pending, m_test,      m_remove};
        for (QWidget *widget : owned) {
            grid->removeWidget(widget);
            widget->deleteLater();
        }
    }

private:
    QString currentUrl() const { return m_destination.reserved ? m_destination.url : m_url->text().trimmed(); }

    QList<QWidget *> unusedWidgets() const
    {
        if (m_destination.reserved) {
            return m_destination.lastFm ? QList<QWidget *>{m_name, m_url, m_token}
                                        : QList<QWidget *>{m_name, m_url, m_fixedToken};
        }
        return {m_fixedName, m_fixedUrl, m_fixedToken};
    }

    void refresh()
    {
        QString text = m_destination.status;
        Health health = m_destination.health;
        if (!isValid()) {
            text = QStringLiteral("Server URL required");
            health = Health::Bad;
        } else if (text.isEmpty()) {
            if (m_destination.lastFm) {
                text = QStringLiteral("Signed in");
                health = Health::Good;
            } else {
                const bool hasToken = !m_token->text().isEmpty();
                text = hasToken ? QStringLiteral("Token set") : QStringLiteral("No token");
                health = hasToken ? Health::Unknown : Health::Bad;
            }
        }
        m_dot->setHealth(health);
        m_status->setText(text);
        setValueOrDash(m_pending, m_destination.pending > 0 ? QString::number(m_destination.pending) : QString());
        m_test->setEnabled(isValid());
        // With no OK button to gate on, an entry that cannot deliver is instead
        // one that cannot be enabled: it is saved, but never sent to.
        if (!isValid() && m_destination.enabled) {
            m_toggle->setChecked(false);
            return;   // the toggle re-enters here
        }
        m_toggle->setEnabled(isValid());
        m_toggle->setToolTip(isValid() ? QStringLiteral("Send listens to this destination")
                                       : QStringLiteral("Give this destination a server address first"));
        m_onChanged();
    }

    Destination m_destination;
    std::function<void()> m_onChanged;
    std::function<void(DestinationRow *)> m_onRemove;
    ToggleSwitch *m_toggle = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_token = nullptr;
    QLabel *m_fixedName = nullptr;
    QLabel *m_fixedUrl = nullptr;
    QLabel *m_fixedToken = nullptr;
    HealthDot *m_dot = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_pending = nullptr;
    QPushButton *m_test = nullptr;
    QPushButton *m_remove = nullptr;
};

QList<Destination> seedDestinations()
{
    return {
        {QStringLiteral("Last.fm"), {}, {}, true, true, true, 0, Health::Good, QStringLiteral("Signed in")},
        {QStringLiteral("ListenBrainz"), QStringLiteral("https://api.listenbrainz.org/1"), QStringLiteral("secret"),
         true, true, false, 12, Health::Good, QStringLiteral("Connected as lobo")},
        {QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), QStringLiteral("secret"), true, false,
         false, 0, Health::Unknown, {}},
        {QStringLiteral("Maloja"), QStringLiteral("https://maloja.example/1"), {}, false, false, false, 3,
         Health::Unknown, {}},
    };
}

QWidget *buildManagerTab(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto *host = new QWidget(page);
    auto *grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(5);

    const QStringList headers{{},
                              QStringLiteral("Name"),
                              QStringLiteral("Server URL"),
                              QStringLiteral("Token"),
                              QStringLiteral("Status"),
                              QStringLiteral("Pending"),
                              {},
                              {}};
    for (int column = 0; column < ManagerColumnCount; ++column) {
        if (headers.at(column).isEmpty()) {
            continue;
        }
        auto *header = new QLabel(headers.at(column), host);
        makeMuted(header);
        header->setAlignment(column == PendingColumn ? Qt::AlignRight | Qt::AlignVCenter
                                                     : Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(header, 0, column);
    }
    grid->addWidget(horizontalRule(host), 1, 0, 1, ManagerColumnCount);
    grid->setColumnStretch(NameColumn, 2);
    grid->setColumnStretch(UrlColumn, 3);

    auto *scroll = new QScrollArea(page);
    scroll->setWidget(host);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);

    auto *problem = new QLabel(page);
    problem->setWordWrap(true);
    layout->addWidget(problem);

    // Add server, offline mode and the Last.fm settings share one row: all three
    // are the dialog's miscellaneous controls, none of them belonging to a
    // single destination. Adding sits under the list it adds to; the two that
    // apply to scrobbling as a whole sit away from it, on the right.
    auto *misc = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add server…"), page);
    auto *offline = new ToggleSwitch(page);
    auto *offlineLabel = new QLabel(QStringLiteral("Offline mode (buffer listens locally)"), page);
    const auto offlineHint = QStringLiteral("Keep collecting listening history but send nothing; switching it off "
                                            "uploads the buffered backlog.");
    offline->setToolTip(offlineHint);
    offlineLabel->setToolTip(offlineHint);
    misc->addWidget(add);
    misc->addStretch();
    misc->addWidget(offline);
    misc->addWidget(offlineLabel);
    misc->addSpacing(16);
    misc->addWidget(new QPushButton(QStringLiteral("Last.fm API settings…"), page));
    layout->addLayout(misc);

    auto *saveNote = new QLabel(QStringLiteral("Changes are saved as you make them, and take effect when this window "
                                               "closes."),
                                page);
    makeMuted(saveNote);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, page);
    auto *footer = new QHBoxLayout;
    footer->addWidget(saveNote);
    footer->addStretch();
    footer->addWidget(buttons);
    layout->addLayout(footer);

    // Rows are appended below the header rule; removing one takes its widgets
    // out of the grid and leaves the rest where they are.
    auto *rows = new QList<DestinationRow *>;
    auto *nextRow = new int(2);
    auto *revalidate = new std::function<void()>;
    *revalidate = [rows, problem] {
        QStringList broken;
        for (DestinationRow *row : *rows) {
            if (!row->isValid()) {
                broken << (row->name().isEmpty() ? QStringLiteral("a new server") : row->name());
            }
        }
        problem->setVisible(!broken.isEmpty());
        problem->setText(broken.isEmpty()
                             ? QString()
                             : QStringLiteral("%1 has no server address, so it cannot be enabled. Give it one "
                                              "or remove it.")
                                   .arg(broken.join(QStringLiteral(", "))));
    };

    auto addRow = [grid, host, rows, nextRow, revalidate](const Destination &destination) {
        auto *entry = new DestinationRow(
            destination, grid, *nextRow, host, [revalidate] { (*revalidate)(); },
            [grid, rows, revalidate](DestinationRow *row) {
                rows->removeAll(row);
                row->takeOutOf(grid);
                row->deleteLater();
                (*revalidate)();
            });
        rows->push_back(entry);
        ++*nextRow;
        (*revalidate)();
    };
    for (const Destination &destination : seedDestinations()) {
        addRow(destination);
    }
    grid->setRowStretch(*nextRow + 64, 1);
    QObject::connect(add, &QPushButton::clicked, page, [addRow] {
        addRow({{}, {}, {}, false, false, false, 0, Health::Unknown, {}});
    });

    return page;
}

// -------------------------------------------------------- history filter tab

// A menu that keeps itself open while checkable entries are toggled, so several
// destinations can be picked in one visit.
class StickyMenu final : public QMenu {
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QAction *action = actionAt(event->position().toPoint());
        if (action != nullptr && action->isCheckable() && action->isEnabled()) {
            action->trigger();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Space) {
            QAction *action = activeAction();
            if (action != nullptr && action->isCheckable() && action->isEnabled()) {
                action->trigger();
                return;
            }
        }
        QMenu::keyPressEvent(event);
    }
};

struct Listen {
    QString time;
    QString title;
    QString artist;
    QStringList sentTo;
    QStringList owedTo;
};

const QStringList kDestinationNames{QStringLiteral("Last.fm"), QStringLiteral("ListenBrainz"),
                                    QStringLiteral("Koito"), QStringLiteral("Maloja")};

class HistoryModel final : public QAbstractTableModel {
public:
    explicit HistoryModel(QObject *parent)
        : QAbstractTableModel(parent)
    {
        for (int index = 0; index < 14; ++index) {
            QStringList sent;
            QStringList owed;
            for (int slot = 0; slot < kDestinationNames.size(); ++slot) {
                if ((index + slot) % 3 != 0) {
                    owed << kDestinationNames.at(slot);
                    if ((index + slot) % 2 == 0) {
                        sent << kDestinationNames.at(slot);
                    }
                }
            }
            m_rows.push_back({QStringLiteral("2026-08-1%1 20:%2").arg(index % 5).arg(10 + index),
                              QStringLiteral("Track %1").arg(index + 1),
                              QStringLiteral("Artist %1").arg((index % 4) + 1), sent, owed});
        }
    }

    void setDestinations(QStringList destinations)
    {
        beginResetModel();
        m_selected = std::move(destinations);
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 4; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        static const QStringList kHeaders{QStringLiteral("Listened"), QStringLiteral("Title"),
                                          QStringLiteral("Artist"), QStringLiteral("Scrobbled")};
        return kHeaders.value(section);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || role != Qt::DisplayRole) {
            return {};
        }
        const Listen &row = m_rows.at(index.row());
        switch (index.column()) {
        case 0:
            return row.time;
        case 1:
            return row.title;
        case 2:
            return row.artist;
        case 3:
            return scrobbledText(row);
        }
        return {};
    }

private:
    // With no destination picked the cell reports every destination that was
    // owed this listen; with a subset picked it reports exactly that subset,
    // which is what makes the count answer the question that was asked.
    QString scrobbledText(const Listen &row) const
    {
        const QStringList scope = m_selected.isEmpty() ? kDestinationNames : m_selected;
        int owed = 0;
        int sent = 0;
        for (const QString &destination : scope) {
            if (row.owedTo.contains(destination)) {
                ++owed;
                if (row.sentTo.contains(destination)) {
                    ++sent;
                }
            }
        }
        if (owed == 0) {
            return QStringLiteral("Not queued");
        }
        if (scope.size() == 1) {
            return sent == 1 ? QStringLiteral("Sent") : QStringLiteral("Pending");
        }
        return QStringLiteral("%1/%2 sent").arg(sent).arg(owed);
    }

    QList<Listen> m_rows;
    QStringList m_selected;
};

// Selection is drawn from the inactive palette group once a popup takes the
// window's activation, and on several themes that inactive highlight is close
// enough to the alternating-row shade to disappear on odd rows. Pinning the
// inactive brushes to the active ones keeps the selection legible while the
// popup is up.
void keepSelectionVisibleWhileUnfocused(QAbstractItemView *view, const QPalette &base, bool pin)
{
    QPalette palette = base;
    if (pin) {
        palette.setBrush(QPalette::Inactive, QPalette::Highlight, base.brush(QPalette::Active, QPalette::Highlight));
        palette.setBrush(QPalette::Inactive, QPalette::HighlightedText,
                         base.brush(QPalette::Active, QPalette::HighlightedText));
    }
    view->setPalette(palette);
    view->viewport()->update();
}

QWidget *buildHistoryTab(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto *model = new HistoryModel(page);
    auto *view = new QTableView(page);
    view->setModel(model);
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->verticalHeader()->setVisible(false);
    view->horizontalHeader()->setStretchLastSection(true);

    auto *button = new QPushButton(page);
    auto *menu = new StickyMenu(button);
    button->setMenu(menu);

    QList<QAction *> destinationActions;
    for (const QString &name : kDestinationNames) {
        QAction *action = menu->addAction(name);
        action->setCheckable(true);
        destinationActions.push_back(action);
    }
    menu->addSeparator();
    QAction *clear = menu->addAction(QStringLiteral("Show all destinations"));

    auto sync = [button, model, destinationActions] {
        QStringList names;
        for (QAction *action : destinationActions) {
            if (action->isChecked()) {
                names << action->text();
            }
        }
        if (names.isEmpty()) {
            button->setText(QStringLiteral("All destinations"));
        } else if (names.size() == 1) {
            button->setText(names.first());
        } else {
            button->setText(QStringLiteral("%1 of %2 destinations").arg(names.size()).arg(kDestinationNames.size()));
        }
        model->setDestinations(names);
    };
    for (QAction *action : destinationActions) {
        QObject::connect(action, &QAction::triggered, page, [sync] { sync(); });
    }
    QObject::connect(clear, &QAction::triggered, page, [destinationActions, sync] {
        for (QAction *action : destinationActions) {
            action->setChecked(false);
        }
        sync();
    });
    sync();

    const QPalette basePalette = view->palette();
    auto *pin = new QCheckBox(QStringLiteral("Keep the table selection visible while the popup is open"), page);
    pin->setChecked(true);
    QObject::connect(pin, &QCheckBox::toggled, page,
                     [view, basePalette](bool on) { keepSelectionVisibleWhileUnfocused(view, basePalette, on); });
    keepSelectionVisibleWhileUnfocused(view, basePalette, true);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Destination"), page));
    top->addWidget(button);
    top->addStretch();
    layout->addLayout(top);
    layout->addWidget(view, 1);
    layout->addWidget(pin);

    auto *hint = new QLabel(QStringLiteral("Select a few rows, then open the popup and toggle destinations: the popup "
                                           "stays up, and the Scrobbled column counts only the destinations picked."),
                            page);
    hint->setWordWrap(true);
    makeMuted(hint);
    layout->addWidget(hint);
    return page;
}

}   // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTabWidget tabs;
    tabs.setWindowTitle(QStringLiteral("muzaiten scrobbling UI prototype"));
    tabs.resize(960, 580);
    tabs.addTab(buildManagerTab(&tabs), QStringLiteral("Scrobblers"));
    tabs.addTab(buildHistoryTab(&tabs), QStringLiteral("Listening history"));
    tabs.show();

    // `build.sh --shot <dir>` renders each tab to a PNG and exits, so a layout
    // change can be looked at without a display attached.
    const QString shotDir = qEnvironmentVariable("SCROBBLER_UI_SHOT_DIR");
    if (!shotDir.isEmpty()) {
        QTimer::singleShot(1200, &app, [&tabs, shotDir] {
            for (int index = 0; index < tabs.count(); ++index) {
                tabs.setCurrentIndex(index);
                QCoreApplication::processEvents();
                tabs.grab().save(QStringLiteral("%1/tab%2.png").arg(shotDir).arg(index + 1));
            }
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
