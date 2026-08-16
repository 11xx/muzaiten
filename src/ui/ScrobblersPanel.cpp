#include "ui/ScrobblersPanel.h"

#include "scrobble/ListenBrainzUrl.h"
#include "scrobble/ListenHistoryStore.h"
#include "ui/ToggleSwitch.h"

#include <QApplication>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

namespace {

// The placeholder for a value a destination cannot have, matching the rest of
// the app's tables.
const auto kEmptyCell = QStringLiteral("—");

enum Column {
    ToggleColumn,
    NameColumn,
    UrlColumn,
    TokenColumn,
    StatusColumn,
    PendingColumn,
    TestColumn,
    RemoveColumn,
    ColumnCount,
};

// Column headers, the empty-value placeholder, and other secondary text. The
// source is always the application palette, so tinting a widget twice does not
// compound.
void setMuted(QWidget *widget, bool muted)
{
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText,
                     QApplication::palette().color(muted ? QPalette::PlaceholderText : QPalette::WindowText));
    widget->setPalette(palette);
}

// A value, or the placeholder for its absence, which recedes rather than
// competing with the values around it.
void setValueOrDash(QLabel *label, const QString &value)
{
    label->setText(value.isEmpty() ? kEmptyCell : value);
    setMuted(label, value.isEmpty());
}

QString mintCustomId()
{
    ScrobbleDestinationSet minted;
    return minted.addCustom({}, {}, false);
}

}   // namespace

// How a destination's connection currently stands, which is what colours the
// dot beside its status text.
enum class DestinationHealth {
    Unknown,   // nothing tried yet
    Good,      // credentials verified
    Bad,       // credentials rejected, or a value the destination cannot work without
    Busy,      // a test is in flight
};

// One destination's controls, laid into a grid shared by every row so the
// columns line up whatever each row happens to offer. The row owns the working
// copy of its destination and reports every change back to the dialog, which
// saves it.
class DestinationRow final : public QObject {
public:
    DestinationRow(ScrobbleDestination destination, ScrobblersPanel *dialog, QGridLayout *grid, int gridRow,
                   QWidget *host, bool alreadyConfigured)
        : QObject(host)
        , m_dialog(dialog)
        , m_destination(std::move(destination))
        , m_alreadyConfigured(alreadyConfigured)
    {
        const bool compatible = m_destination.type == ScrobbleDestination::Type::ListenBrainzCompatible;
        const bool reserved = m_destination.isReserved();

        m_toggle = new ToggleSwitch(host);
        m_toggle->setChecked(m_destination.enabled);
        connect(m_toggle, &QAbstractButton::toggled, this, [this](bool on) {
            if (m_destination.enabled == on) {
                return;
            }
            m_destination.enabled = on;
            refresh();
            m_dialog->save();
        });

        m_name = new QLineEdit(m_destination.name, host);
        m_name->setPlaceholderText(QStringLiteral("Koito"));
        connect(m_name, &QLineEdit::editingFinished, this, &DestinationRow::commitName);

        m_url = new QLineEdit(m_destination.apiRoot, host);
        m_url->setPlaceholderText(QStringLiteral("https://koito.example"));
        m_url->setToolTip(QStringLiteral("Both the site root and its /1 API root work; muzaiten normalizes them to "
                                         "the same server."));
        connect(m_url, &QLineEdit::editingFinished, this, &DestinationRow::commitUrl);
        connect(m_url, &QLineEdit::textChanged, this, [this] {
            m_status.clear();
            refresh();
        });

        m_token = new QLineEdit(compatible ? m_dialog->m_callbacks.readToken(m_destination.id) : QString(), host);
        m_token->setEchoMode(QLineEdit::Password);
        m_token->setPlaceholderText(QStringLiteral("user token"));
        connect(m_token, &QLineEdit::editingFinished, this, &DestinationRow::commitToken);
        connect(m_token, &QLineEdit::textChanged, this, [this] {
            m_status.clear();
            refresh();
        });

        // A built-in destination is not the user's to rename or repoint, so it
        // shows the value as text rather than as a field that refuses input.
        m_fixedName = new QLabel(m_destination.name, host);
        m_fixedUrl = new QLabel(host);
        m_fixedUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setValueOrDash(m_fixedUrl, compatible ? m_destination.apiRoot : QString());
        m_fixedToken = new QLabel(host);
        setValueOrDash(m_fixedToken, {});
        m_fixedToken->setToolTip(QStringLiteral("Last.fm signs in through Last.fm API settings"));
        // Indented to the text inset of the fields beside them, so a fixed value
        // and an editable one start at the same x.
        const int inset = host->style()->pixelMetric(QStyle::PM_DefaultFrameWidth) + 3;
        for (QLabel *fixed : {m_fixedName, m_fixedUrl, m_fixedToken}) {
            fixed->setTextFormat(Qt::PlainText);
            fixed->setIndent(inset);
        }

        m_dot = new HealthDot(host);
        m_statusLabel = new QLabel(host);
        m_pending = new QLabel(host);
        m_pending->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_test = new QPushButton(QStringLiteral("Test"), host);
        m_test->setToolTip(QStringLiteral("Check the server and credentials"));
        connect(m_test, &QPushButton::clicked, this, &DestinationRow::test);

        m_remove = new QPushButton(host);
        m_remove->setIcon(
            QIcon::fromTheme(QStringLiteral("user-trash"), host->style()->standardIcon(QStyle::SP_TrashIcon)));
        m_remove->setToolTip(QStringLiteral("Remove this destination"));
        connect(m_remove, &QPushButton::clicked, this, [this] { m_dialog->removeRow(this); });

        // Each control is addressable by the destination it belongs to, which is
        // what lets anything outside this row reach one without counting
        // children in layout order.
        m_toggle->setObjectName(m_destination.id + QStringLiteral(".enabled"));
        m_name->setObjectName(m_destination.id + QStringLiteral(".name"));
        m_url->setObjectName(m_destination.id + QStringLiteral(".url"));
        m_token->setObjectName(m_destination.id + QStringLiteral(".token"));
        m_statusLabel->setObjectName(m_destination.id + QStringLiteral(".status"));
        m_test->setObjectName(m_destination.id + QStringLiteral(".test"));
        m_remove->setObjectName(m_destination.id + QStringLiteral(".remove"));

        m_statusLayout = new QHBoxLayout;
        QHBoxLayout *status = m_statusLayout;
        status->setContentsMargins(0, 0, 0, 0);
        status->setSpacing(4);
        status->addWidget(m_dot);
        status->addWidget(m_statusLabel);
        status->addStretch();

        grid->addWidget(m_toggle, gridRow, ToggleColumn);
        grid->addWidget(reserved ? static_cast<QWidget *>(m_fixedName) : m_name, gridRow, NameColumn);
        grid->addWidget(reserved ? static_cast<QWidget *>(m_fixedUrl) : m_url, gridRow, UrlColumn);
        grid->addWidget(compatible ? static_cast<QWidget *>(m_token) : m_fixedToken, gridRow, TokenColumn);
        grid->addLayout(status, gridRow, StatusColumn);
        grid->addWidget(m_pending, gridRow, PendingColumn);
        grid->addWidget(m_test, gridRow, TestColumn);
        grid->addWidget(m_remove, gridRow, RemoveColumn);

        for (QWidget *unused : unusedWidgets()) {
            unused->hide();
        }
        m_test->setVisible(compatible);
        m_remove->setVisible(!reserved);
        // A row that offers fewer actions still reserves their space, so the
        // action columns stay columns.
        for (QPushButton *button : {m_test, m_remove}) {
            QSizePolicy policy = button->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            button->setSizePolicy(policy);
        }

        refresh();
    }

    const ScrobbleDestination &destination() const { return m_destination; }

    void setEnabled(bool enabled)
    {
        if (m_destination.enabled == enabled) {
            return;
        }
        m_destination.enabled = enabled;
        const QSignalBlocker blocker(m_toggle);
        m_toggle->setChecked(enabled);
        refresh();
    }

    // A ListenBrainz-compatible destination with no address has nowhere to
    // deliver, which is fatal for that entry rather than a warning about it.
    bool isDeliverable() const { return !m_destination.apiRoot.isEmpty() || !isCompatible(); }

    QString displayName() const
    {
        const QString name = m_destination.name.trimmed();
        return name.isEmpty() ? QStringLiteral("the new server") : name;
    }

    void setStatus(const QString &status, DestinationHealth health)
    {
        m_status = status;
        m_health = health;
        refresh();
    }

    bool isTesting() const { return m_health == DestinationHealth::Busy; }

    void focusFirstField() { m_url->setFocus(); }

    void takeOutOf(QGridLayout *grid)
    {
        // The status cell is a nested layout, not a widget, so removing the
        // widgets alone would leave its item behind in the grid.
        grid->removeItem(m_statusLayout);
        delete m_statusLayout;
        m_statusLayout = nullptr;

        const QList<QWidget *> owned{m_toggle,     m_name, m_url,        m_token,   m_fixedName, m_fixedUrl,
                                     m_fixedToken, m_dot,  m_statusLabel, m_pending, m_test,      m_remove};
        for (QWidget *widget : owned) {
            grid->removeWidget(widget);
            widget->deleteLater();
        }
    }

private:
    // A dot ahead of the status word, sized to sit on the text's baseline.
    class HealthDot final : public QWidget {
    public:
        explicit HealthDot(QWidget *parent)
            : QWidget(parent)
        {
            setFixedWidth(10);
        }

        void setHealth(DestinationHealth health)
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
            case DestinationHealth::Good:
                return QColor(0x3f, 0xa0, 0x5f);
            case DestinationHealth::Bad:
                return QColor(0xc6, 0x4b, 0x4b);
            case DestinationHealth::Busy:
                return palette().color(QPalette::Highlight);
            case DestinationHealth::Unknown:
                break;
            }
            return palette().color(QPalette::PlaceholderText);
        }

        DestinationHealth m_health = DestinationHealth::Unknown;
    };

    bool isCompatible() const { return m_destination.type == ScrobbleDestination::Type::ListenBrainzCompatible; }

    QList<QWidget *> unusedWidgets() const
    {
        if (m_destination.isReserved()) {
            return isCompatible() ? QList<QWidget *>{m_name, m_url, m_fixedToken}
                                  : QList<QWidget *>{m_name, m_url, m_token};
        }
        return {m_fixedName, m_fixedUrl, m_fixedToken};
    }

    void commitName()
    {
        const QString typed = m_name->text().trimmed();
        if (typed == m_destination.name) {
            return;
        }
        m_destination.name = typed;
        m_dialog->save();
    }

    // Returns whether the field now holds an address this destination can use.
    bool commitUrl()
    {
        const QString typed = m_url->text().trimmed();
        if (typed.isEmpty()) {
            if (m_alreadyConfigured) {
                // Dropping the address of a configured destination would take it
                // out of the configuration and strand its delivery records.
                // Removing it is the way to be rid of it, and that says what it
                // discards first.
                m_url->setText(m_destination.apiRoot);
                setStatus(QStringLiteral("An address is required; remove the destination instead"),
                          DestinationHealth::Bad);
                return true;
            }
            m_destination.apiRoot.clear();
            refresh();
            m_dialog->save();
            return false;
        }

        const ListenBrainzUrl::Normalized result = ListenBrainzUrl::normalizeBase(typed);
        if (!result.valid) {
            setStatus(result.error, DestinationHealth::Bad);
            return false;
        }
        if (result.apiRoot == m_destination.apiRoot) {
            m_url->setText(result.apiRoot);
            return true;
        }

        const int pending = m_dialog->m_history != nullptr ? m_dialog->m_history->pendingCount(m_destination.id) : 0;
        if (pending > 0) {
            // The backlog belongs to the destination, not to the URL, so it
            // follows the destination to its new address. Say so before it does.
            const auto answer = QMessageBox::question(
                m_dialog, QStringLiteral("Change server URL"),
                QStringLiteral("\"%1\" has %2 listens waiting to be sent. They stay queued and will be delivered to "
                               "the new address instead. Continue?")
                    .arg(displayName())
                    .arg(pending),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                m_url->setText(m_destination.apiRoot);
                return true;
            }
        }

        const bool wasUndeliverable = !isDeliverable();
        m_destination.apiRoot = result.apiRoot;
        m_url->setText(result.apiRoot);
        // The name defaults to the address until the user gives it one of its own.
        if (m_destination.name.isEmpty()) {
            m_destination.name = result.apiRoot;
            m_name->setText(result.apiRoot);
        }
        m_status.clear();
        refresh();
        m_dialog->save();
        if (wasUndeliverable) {
            // Any token typed while there was nowhere to send it was held back;
            // it has somewhere now.
            commitToken();
        }
        return true;
    }

    void commitToken()
    {
        if (!isCompatible()) {
            return;
        }
        // A destination with nowhere to send is not in the configuration, so
        // storing a token against its id would leave one behind that only an
        // explicit removal would clean up. It is held until there is an address
        // to go with it.
        if (!isDeliverable()) {
            return;
        }
        const QString typed = m_token->text().trimmed();
        if (typed == m_dialog->m_callbacks.readToken(m_destination.id)) {
            return;
        }
        m_dialog->m_callbacks.writeToken(m_destination.id, typed);
        refresh();
    }

    void test()
    {
        if (!commitUrl() || !isDeliverable()) {
            return;
        }
        commitToken();
        setStatus(QStringLiteral("Testing…"), DestinationHealth::Busy);
        m_dialog->m_callbacks.testDestination(m_destination.id, m_destination.apiRoot, m_token->text().trimmed());
    }

    void refresh()
    {
        QString text = m_status;
        DestinationHealth health = m_health;
        if (!isDeliverable()) {
            text = QStringLiteral("Server URL required");
            health = DestinationHealth::Bad;
        } else if (text.isEmpty()) {
            if (!isCompatible()) {
                const bool configured = m_dialog->m_callbacks.lastFmConfigured();
                text = configured ? QStringLiteral("Signed in") : QStringLiteral("Not signed in");
                health = configured ? DestinationHealth::Good : DestinationHealth::Bad;
            } else {
                const bool hasToken = !m_token->text().trimmed().isEmpty();
                text = hasToken ? QStringLiteral("Token set") : QStringLiteral("No token");
                health = hasToken ? DestinationHealth::Unknown : DestinationHealth::Bad;
            }
        }
        m_dot->setHealth(health);
        m_statusLabel->setText(text);

        const int pending = m_dialog->m_history != nullptr ? m_dialog->m_history->pendingCount(m_destination.id) : 0;
        setValueOrDash(m_pending, pending > 0 ? QString::number(pending) : QString());

        m_test->setEnabled(isDeliverable());
        // With no accept button to gate on, an entry that cannot deliver is
        // instead one that cannot be enabled: it is kept, but never sent to.
        if (!isDeliverable() && m_toggle->isChecked()) {
            m_toggle->setChecked(false);   // re-enters here through the toggle
            return;
        }
        m_toggle->setEnabled(isDeliverable());
        m_toggle->setToolTip(isDeliverable() ? QStringLiteral("Send listens to this destination")
                                             : QStringLiteral("Give this destination a server address first"));
        m_dialog->updateNotices();
    }

    ScrobblersPanel *m_dialog = nullptr;
    ScrobbleDestination m_destination;
    // Whether this destination is already part of the saved configuration, as
    // opposed to a row added here that has not earned an address yet.
    bool m_alreadyConfigured = false;
    QHBoxLayout *m_statusLayout = nullptr;
    QString m_status;
    DestinationHealth m_health = DestinationHealth::Unknown;

    ToggleSwitch *m_toggle = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_token = nullptr;
    QLabel *m_fixedName = nullptr;
    QLabel *m_fixedUrl = nullptr;
    QLabel *m_fixedToken = nullptr;
    HealthDot *m_dot = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_pending = nullptr;
    QPushButton *m_test = nullptr;
    QPushButton *m_remove = nullptr;
};

ScrobblersPanel::ScrobblersPanel(ScrobbleDestinationSet destinations, ListenHistoryStore *history,
                                                       Callbacks callbacks, QWidget *parent)
    : QWidget(parent)
    , m_history(history)
    , m_callbacks(std::move(callbacks))
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    m_rowHost = new QWidget(this);
    m_grid = new QGridLayout(m_rowHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(10);
    m_grid->setVerticalSpacing(5);

    const QStringList headers{{},
                              QStringLiteral("Name"),
                              QStringLiteral("Server URL"),
                              QStringLiteral("Token"),
                              QStringLiteral("Status"),
                              QStringLiteral("Pending"),
                              {},
                              {}};
    for (int column = 0; column < ColumnCount; ++column) {
        if (headers.at(column).isEmpty()) {
            continue;
        }
        auto *header = new QLabel(headers.at(column), m_rowHost);
        setMuted(header, true);
        header->setAlignment(column == PendingColumn ? Qt::AlignRight | Qt::AlignVCenter
                                                     : Qt::AlignLeft | Qt::AlignVCenter);
        m_grid->addWidget(header, 0, column);
    }
    auto *rule = new QFrame(m_rowHost);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    m_grid->addWidget(rule, 1, 0, 1, ColumnCount);
    m_grid->setColumnStretch(NameColumn, 2);
    m_grid->setColumnStretch(UrlColumn, 3);
    m_nextGridRow = 2;

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(m_rowHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);

    m_problem = new QLabel(this);
    m_problem->setWordWrap(true);
    layout->addWidget(m_problem);

    m_relayWarning = new QLabel(this);
    m_relayWarning->setWordWrap(true);
    setMuted(m_relayWarning, true);
    layout->addWidget(m_relayWarning);

    // Add server, offline mode and the Last.fm settings share one row: all three
    // are controls of the dialog, none of them belonging to a single
    // destination. Adding sits under the list it adds to; the two that apply to
    // scrobbling as a whole sit away from it, on the right.
    auto *misc = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add server…"), this);
    connect(add, &QPushButton::clicked, this, &ScrobblersPanel::addDestination);

    m_offline = new ToggleSwitch(this);
    m_offline->setChecked(m_callbacks.readOffline ? m_callbacks.readOffline() : false);
    auto *offlineLabel = new QLabel(QStringLiteral("Offline mode (buffer listens locally)"), this);
    const auto offlineHint = QStringLiteral("Keep collecting listening history but send nothing; switching it off "
                                            "uploads the buffered backlog.");
    m_offline->setToolTip(offlineHint);
    offlineLabel->setToolTip(offlineHint);
    connect(m_offline, &QAbstractButton::toggled, this, [this](bool offline) {
        if (m_callbacks.writeOffline) {
            m_callbacks.writeOffline(offline);
        }
    });

    auto *lastFmSettings = new QPushButton(QStringLiteral("Last.fm API settings…"), this);
    connect(lastFmSettings, &QPushButton::clicked, this, [this] {
        if (m_callbacks.openLastFmSettings) {
            m_callbacks.openLastFmSettings();
        }
        updateNotices();
    });

    misc->addWidget(add);
    misc->addStretch();
    misc->addWidget(m_offline);
    misc->addWidget(offlineLabel);
    misc->addSpacing(16);
    misc->addWidget(lastFmSettings);
    layout->addLayout(misc);

    auto *saveNote = new QLabel(
        QStringLiteral("Changes are saved as you make them, and take effect when this window closes."), this);
    setMuted(saveNote, true);
    layout->addWidget(saveNote);

    for (const ScrobbleDestination &destination : destinations.items) {
        appendRow(destination, true);
    }
    m_grid->setRowStretch(m_nextGridRow + 64, 1);
    updateNotices();
}

void ScrobblersPanel::appendRow(const ScrobbleDestination &destination, bool alreadyConfigured)
{
    m_rows.push_back(new DestinationRow(destination, this, m_grid, m_nextGridRow, m_rowHost, alreadyConfigured));
    ++m_nextGridRow;
}

ScrobbleDestinationSet ScrobblersPanel::destinations() const
{
    ScrobbleDestinationSet set;
    for (const DestinationRow *row : m_rows) {
        // An entry still missing its address is not a destination yet, so it is
        // held in the dialog rather than written to the configuration.
        if (row->isDeliverable()) {
            set.items.push_back(row->destination());
        }
    }
    return set;
}

void ScrobblersPanel::save()
{
    const ScrobbleDestinationSet saved = destinations();
    if (m_callbacks.saveDestinations) {
        m_callbacks.saveDestinations(saved);
    }
    updateNotices();
    emit destinationsChanged(saved);
}

void ScrobblersPanel::addDestination()
{
    ScrobbleDestination destination;
    destination.id = mintCustomId();
    destination.type = ScrobbleDestination::Type::ListenBrainzCompatible;
    appendRow(destination, false);
    m_rows.last()->focusFirstField();
    updateNotices();
}

void ScrobblersPanel::removeRow(DestinationRow *row)
{
    const ScrobbleDestination destination = row->destination();
    const int pending = m_history != nullptr ? m_history->pendingCount(destination.id) : 0;
    const QString detail = pending > 0
        ? QStringLiteral("\"%1\" has %2 listens waiting to be sent. Removing it discards them.")
              .arg(row->displayName())
              .arg(pending)
        : QStringLiteral("Remove \"%1\"?").arg(row->displayName());
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Remove scrobbler"),
        detail
            + QStringLiteral("\n\nThis cannot be undone. Your local listening history is kept; only this "
                             "destination's delivery records and stored token are deleted."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    m_rows.removeAll(row);
    row->takeOutOf(m_grid);
    row->deleteLater();
    if (m_history != nullptr) {
        m_history->forgetDestination(destination.id);
    }
    m_callbacks.removeToken(destination.id);
    save();
}

void ScrobblersPanel::reportTestResult(const QString &destinationId, bool valid, const QString &username)
{
    for (DestinationRow *row : std::as_const(m_rows)) {
        // A result that no longer answers a live test describes an address or a
        // token the row has since moved on from.
        if (row->destination().id == destinationId && row->isTesting()) {
            row->setStatus(valid ? QStringLiteral("Connected as %1").arg(username)
                                 : QStringLiteral("Token rejected"),
                           valid ? DestinationHealth::Good : DestinationHealth::Bad);
            return;
        }
    }
}

void ScrobblersPanel::adoptEnabledState(const QString &destinationId, bool enabled)
{
    for (DestinationRow *row : std::as_const(m_rows)) {
        if (row->destination().id == destinationId) {
            row->setEnabled(enabled);
            return;
        }
    }
}

void ScrobblersPanel::updateNotices()
{
    QStringList incomplete;
    int enabledCompatible = 0;
    for (const DestinationRow *row : std::as_const(m_rows)) {
        if (!row->isDeliverable()) {
            incomplete << row->displayName();
            continue;
        }
        const ScrobbleDestination &destination = row->destination();
        if (destination.enabled && destination.type == ScrobbleDestination::Type::ListenBrainzCompatible) {
            ++enabledCompatible;
        }
    }

    m_problem->setVisible(!incomplete.isEmpty());
    m_problem->setText(incomplete.isEmpty()
                           ? QString()
                           : QStringLiteral("%1 has no server address, so it cannot be enabled. Give it one or "
                                            "remove it.")
                                 .arg(incomplete.join(QStringLiteral(", "))));

    // Informational, never blocking: relaying to several servers is a legitimate
    // setup, it just means each listen is submitted more than once.
    m_relayWarning->setVisible(enabledCompatible > 1);
    m_relayWarning->setText(QStringLiteral("%1 ListenBrainz-compatible destinations are enabled. Every listen is "
                                           "submitted to each of them, so any server that relays to another may "
                                           "receive it twice.")
                                .arg(enabledCompatible));
}
