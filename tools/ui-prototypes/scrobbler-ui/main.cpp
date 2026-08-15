// Standalone prototype for two scrobbling UI surfaces, built and run on its own
// so the design can be iterated without rebuilding the application:
//
//   * the scrobbler manager as a list of per-destination rows, where every
//     action is reachable in one click and the fields edit in place;
//   * the listening-history destination filter as a multi-select popup that
//     stays open while destinations are toggled, next to a table that keeps its
//     selection visible while that popup holds focus.
//
// It carries fake data and no persistence. Nothing here is linked into the
// application; see tools/ui-prototypes/README.md.

#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEnterEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <array>

namespace {

// ---------------------------------------------------------------- shared bits

enum class Health {
    Unknown,   // nothing tried yet
    Good,      // credentials verified
    Bad,       // credentials rejected
    Busy,      // a test is in flight
};

QColor healthColor(Health health, const QPalette &palette)
{
    switch (health) {
    case Health::Good:
        return QColor(0x3f, 0xa0, 0x5f);
    case Health::Bad:
        return QColor(0xc6, 0x4b, 0x4b);
    case Health::Busy:
        return palette.color(QPalette::Highlight);
    case Health::Unknown:
        break;
    }
    return palette.color(QPalette::Mid);
}

// Secondary text: readable, but clearly subordinate to the value beside it.
// PlaceholderText tracks the theme, which a stylesheet colour cannot.
void makeMuted(QWidget *widget)
{
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    palette.setColor(QPalette::Text, palette.color(QPalette::PlaceholderText));
    widget->setPalette(palette);
}

// A dot plus a word, sized like body text: enough to read the state at a glance
// without the weight of a table cell.
class StatusPill final : public QWidget {
public:
    explicit StatusPill(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void setState(Health health, const QString &text)
    {
        m_health = health;
        m_text = text;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(font());
        return {metrics.horizontalAdvance(m_text) + 18, metrics.height() + 4};
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor color = healthColor(m_health, palette());
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const int dot = 7;
        painter.drawEllipse(QPointF(dot / 2.0 + 1, height() / 2.0), dot / 2.0, dot / 2.0);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRect(dot + 6, 0, width() - dot - 6, height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
    }

private:
    Health m_health = Health::Unknown;
    QString m_text;
};

// A switch rather than a checkbox: it reads as a live state that a single click
// flips, which is what enabling a destination is.
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

        const QRectF track(1, (height() - 18) / 2.0, 36, 18);
        const QColor off = palette().color(QPalette::Mid);
        const QColor on = palette().color(QPalette::Highlight);
        const auto blend = [this](qreal from, qreal to) { return static_cast<float>(from + (to - from) * m_position); };
        QColor trackColor = QColor::fromRgbF(blend(off.redF(), on.redF()), blend(off.greenF(), on.greenF()),
                                             blend(off.blueF(), on.blueF()));
        if (!isEnabled()) {
            trackColor = palette().color(QPalette::Disabled, QPalette::Mid);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(trackColor);
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);

        if (hasFocus()) {
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(track.adjusted(-1, -1, 1, 1), track.height() / 2, track.height() / 2);
        }

        const qreal travel = track.width() - track.height();
        const QPointF knob(track.left() + track.height() / 2 + travel * m_position, track.center().y());
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::BrightText));
        painter.drawEllipse(knob, track.height() / 2 - 2.5, track.height() / 2 - 2.5);
    }

private:
    QVariantAnimation *m_slide = nullptr;
    qreal m_position = 0.0;
};

// A field that reads as text until it is pointed at, so a row of values does not
// look like a form yet still edits where it is displayed.
class InlineEdit final : public QLineEdit {
public:
    explicit InlineEdit(const QString &text, QWidget *parent = nullptr)
        : QLineEdit(text, parent)
    {
        setFrame(false);
        applySkin(Skin::Resting);
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QLineEdit::enterEvent(event);
        if (isEnabled() && !isReadOnly() && !hasFocus()) {
            applySkin(Skin::Hover);
        }
    }

    void leaveEvent(QEvent *event) override
    {
        QLineEdit::leaveEvent(event);
        if (!hasFocus()) {
            applySkin(Skin::Resting);
        }
    }

    void focusInEvent(QFocusEvent *event) override
    {
        QLineEdit::focusInEvent(event);
        applySkin(isReadOnly() ? Skin::Resting : Skin::Editing);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QLineEdit::focusOutEvent(event);
        applySkin(Skin::Resting);
    }

private:
    enum class Skin { Resting, Hover, Editing };

    void applySkin(Skin skin)
    {
        static const auto kResting = QStringLiteral(
            "QLineEdit{background:transparent;border:1px solid transparent;border-radius:3px;padding:1px 3px;}");
        static const auto kHover = QStringLiteral(
            "QLineEdit{background:transparent;border:1px solid palette(mid);border-radius:3px;padding:1px 3px;}");
        static const auto kEditing = QStringLiteral(
            "QLineEdit{background:palette(base);border:1px solid palette(highlight);border-radius:3px;padding:1px 3px;}");
        switch (skin) {
        case Skin::Resting:
            setStyleSheet(kResting);
            return;
        case Skin::Hover:
            setStyleSheet(kHover);
            return;
        case Skin::Editing:
            setStyleSheet(kEditing);
            return;
        }
    }
};

// A borderless button that only draws itself once the row is pointed at, so the
// per-row actions stay available without turning the list into a toolbar.
QToolButton *rowAction(const QString &text, const QString &tip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral("QToolButton{border:1px solid transparent;border-radius:3px;padding:2px 8px;}"
                                         "QToolButton:hover{border-color:palette(mid);}"));
    // A row that offers fewer actions still reserves their space, so the action
    // column stays a column.
    QSizePolicy policy = button->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    button->setSizePolicy(policy);
    return button;
}

// ------------------------------------------------------- scrobbler manager tab

struct Destination {
    QString name;
    QString url;
    QString token;
    bool enabled = false;
    bool reserved = false;   // Last.fm and official ListenBrainz: name and URL are not the user's
    bool lastFm = false;
    int pending = 0;
    Health health = Health::Unknown;
    QString status;
};

class DestinationRow final : public QFrame {
public:
    DestinationRow(Destination destination, QWidget *parent)
        : QFrame(parent)
        , m_destination(std::move(destination))
    {
        setObjectName(QStringLiteral("destinationRow"));
        setFrameShape(QFrame::StyledPanel);
        setAttribute(Qt::WA_Hover);

        m_toggle = new ToggleSwitch(this);
        m_toggle->setChecked(m_destination.enabled);
        m_toggle->setToolTip(QStringLiteral("Send listens to this destination"));
        connect(m_toggle, &QAbstractButton::toggled, this, [this](bool on) {
            m_destination.enabled = on;
            refresh();
        });

        m_name = new InlineEdit(m_destination.name, this);
        QFont nameFont = m_name->font();
        nameFont.setBold(true);
        m_name->setFont(nameFont);
        m_name->setReadOnly(m_destination.reserved);
        m_name->setToolTip(m_destination.reserved ? QStringLiteral("Built in: this destination cannot be renamed")
                                                  : QStringLiteral("Click to rename"));

        m_url = new InlineEdit(m_destination.lastFm ? QString() : m_destination.url, this);
        m_url->setPlaceholderText(m_destination.lastFm ? QStringLiteral("Authenticated in Last.fm settings")
                                                       : QStringLiteral("https://koito.example"));
        m_url->setReadOnly(m_destination.reserved);

        m_token = new InlineEdit(m_destination.token, this);
        m_token->setEchoMode(QLineEdit::Password);
        m_token->setPlaceholderText(QStringLiteral("user token"));
        m_token->setMaximumWidth(150);
        // Last.fm authenticates through its own settings dialog, so a token
        // field here would be an empty promise.
        m_token->setVisible(!m_destination.lastFm);

        m_pill = new StatusPill(this);
        m_pending = new QLabel(this);
        makeMuted(m_pending);

        m_test = rowAction(QStringLiteral("Test"), QStringLiteral("Check the server and credentials"), this);
        m_test->setVisible(!m_destination.lastFm);
        m_remove = rowAction(QStringLiteral("✕"), QStringLiteral("Remove this destination"), this);
        // A built-in destination cannot be removed, so it offers no control to
        // grey out.
        m_remove->setVisible(!m_destination.reserved);
        connect(m_test, &QToolButton::clicked, this, [this] {
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
        connect(m_remove, &QToolButton::clicked, this, [this] { deleteLater(); });

        auto *identity = new QVBoxLayout;
        identity->setSpacing(1);
        identity->addWidget(m_name);
        auto *endpoint = new QHBoxLayout;
        endpoint->setSpacing(6);
        m_url->setMinimumWidth(240);
        endpoint->addWidget(m_url, 0);
        auto *tokenLabel = new QLabel(QStringLiteral("Token"), this);
        makeMuted(tokenLabel);
        tokenLabel->setVisible(!m_destination.lastFm);
        endpoint->addWidget(tokenLabel);
        endpoint->addWidget(m_token);
        endpoint->addStretch();
        identity->addLayout(endpoint);

        auto *right = new QVBoxLayout;
        right->setSpacing(1);
        right->addWidget(m_pill, 0, Qt::AlignRight);
        right->addWidget(m_pending, 0, Qt::AlignRight);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 7, 8, 7);
        layout->setSpacing(10);
        layout->addWidget(m_toggle, 0, Qt::AlignVCenter);
        layout->addLayout(identity, 1);
        layout->addLayout(right);
        layout->addWidget(m_test, 0, Qt::AlignVCenter);
        layout->addWidget(m_remove, 0, Qt::AlignVCenter);

        refresh();
    }

    bool enabled() const { return m_destination.enabled; }
    bool compatible() const { return !m_destination.lastFm; }

private:
    void refresh()
    {
        const bool on = m_destination.enabled;
        QString status = m_destination.status;
        Health health = m_destination.health;
        if (status.isEmpty()) {
            if (m_destination.lastFm) {
                status = QStringLiteral("Signed in");
                health = Health::Good;
            } else {
                status = m_token->text().isEmpty() ? QStringLiteral("No token") : QStringLiteral("Token set");
                health = m_token->text().isEmpty() ? Health::Bad : Health::Unknown;
            }
        }
        m_pill->setState(health, status);
        m_pending->setText(m_destination.pending > 0
                               ? QStringLiteral("%1 waiting to send").arg(m_destination.pending)
                               : QString());
        // An enabled destination is outlined in the highlight colour, so the
        // list reads as "these are live" without anything having to be selected.
        setStyleSheet(on
            ? QStringLiteral("#destinationRow{border:1px solid palette(highlight);border-radius:5px;}")
            : QStringLiteral("#destinationRow{border:1px solid palette(mid);border-radius:5px;}"));
    }

    Destination m_destination;
    ToggleSwitch *m_toggle = nullptr;
    InlineEdit *m_name = nullptr;
    InlineEdit *m_url = nullptr;
    InlineEdit *m_token = nullptr;
    StatusPill *m_pill = nullptr;
    QLabel *m_pending = nullptr;
    QToolButton *m_test = nullptr;
    QToolButton *m_remove = nullptr;
};

QWidget *buildManagerTab(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *lede = new QLabel(QStringLiteral("Every listen is sent to each enabled destination."), page);
    makeMuted(lede);
    layout->addWidget(lede);

    auto *host = new QWidget(page);
    auto *rows = new QVBoxLayout(host);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(6);

    const std::array<Destination, 4> seed{{
        {QStringLiteral("Last.fm"), {}, {}, true, true, true, 0, Health::Good, QStringLiteral("Signed in")},
        {QStringLiteral("ListenBrainz"), QStringLiteral("https://api.listenbrainz.org/1"),
         QStringLiteral("secret"), true, true, false, 12, Health::Good, QStringLiteral("Connected as lobo")},
        {QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), QStringLiteral("secret"), true, false,
         false, 0, Health::Unknown, {}},
        {QStringLiteral("Maloja"), QStringLiteral("https://maloja.example/1"), {}, false, false, false, 3,
         Health::Unknown, {}},
    }};
    for (const Destination &destination : seed) {
        rows->addWidget(new DestinationRow(destination, host));
    }
    rows->addStretch();

    auto *scroll = new QScrollArea(page);
    scroll->setWidget(host);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);

    auto *warning = new QLabel(QStringLiteral("3 ListenBrainz-compatible destinations are enabled. Every listen is "
                                              "submitted to each of them, so any server that relays to another may "
                                              "receive it twice."),
                               page);
    warning->setWordWrap(true);
    makeMuted(warning);
    layout->addWidget(warning);

    auto *addRow = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add server…"), page);
    QObject::connect(add, &QPushButton::clicked, page, [rows, host] {
        rows->insertWidget(rows->count() - 1,
                           new DestinationRow(Destination{QStringLiteral("New server"), {}, {}, false, false, false, 0,
                                                          Health::Unknown, {}},
                                              host));
    });
    addRow->addWidget(add);
    addRow->addStretch();
    layout->addLayout(addRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, page);
    layout->addWidget(buttons);
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

// Selection is drawn with the inactive palette group once a popup takes focus,
// and on several styles that inactive highlight is close enough to the
// alternating-row shade to disappear on odd rows. Pinning the inactive brushes
// to the active ones keeps the selection legible for as long as the popup is up.
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
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *model = new HistoryModel(page);
    auto *view = new QTableView(page);
    view->setModel(model);
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->verticalHeader()->setVisible(false);
    view->horizontalHeader()->setStretchLastSection(true);

    auto *button = new QToolButton(page);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
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

    auto selected = [destinationActions] {
        QStringList names;
        for (QAction *action : destinationActions) {
            if (action->isChecked()) {
                names << action->text();
            }
        }
        return names;
    };
    auto sync = [button, model, selected] {
        const QStringList names = selected();
        if (names.isEmpty()) {
            button->setText(QStringLiteral("All destinations  ▾"));
        } else if (names.size() == 1) {
            button->setText(names.first() + QStringLiteral("  ▾"));
        } else {
            button->setText(QStringLiteral("%1 of %2 destinations  ▾").arg(names.size()).arg(kDestinationNames.size()));
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
    tabs.resize(860, 520);
    tabs.addTab(buildManagerTab(&tabs), QStringLiteral("Scrobblers"));
    tabs.addTab(buildHistoryTab(&tabs), QStringLiteral("Listening history filter"));
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
