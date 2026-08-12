#include "ui/ScrobbleDestinationsDialog.h"

#include "scrobble/ListenBrainzUrl.h"
#include "scrobble/ListenHistoryStore.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Column {
    NameColumn,
    TypeColumn,
    UrlColumn,
    EnabledColumn,
    StatusColumn,
    PendingColumn,
    ColumnCount,
};

// The table's placeholder for a cell that has no value, matching the rest of
// the app's tables.
const auto kEmptyCell = QStringLiteral("—");

// Collects name, URL, and token for one compatible destination, refusing to
// accept anything the URL normalizer rejects.
class CompatibleDestinationDialog final : public QDialog {
public:
    CompatibleDestinationDialog(const QString &title, const QString &name, const QString &apiRoot,
                                const QString &token, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(title);

        m_name = new QLineEdit(name, this);
        m_name->setPlaceholderText(QStringLiteral("Koito"));
        m_url = new QLineEdit(apiRoot, this);
        m_url->setPlaceholderText(QStringLiteral("https://koito.example"));
        m_token = new QLineEdit(token, this);
        m_token->setEchoMode(QLineEdit::Password);

        m_error = new QLabel(this);
        m_error->setWordWrap(true);
        m_error->setStyleSheet(QStringLiteral("color: palette(link-visited);"));
        m_error->hide();

        auto *form = new QFormLayout;
        form->addRow(QStringLiteral("Name"), m_name);
        form->addRow(QStringLiteral("Server URL"), m_url);
        form->addRow(QStringLiteral("User token"), m_token);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &CompatibleDestinationDialog::validateAndAccept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(QStringLiteral("Enter the server's address. Both the site root and its /1 API "
                                                    "root work; muzaiten normalizes them to the same server."),
                                     this));
        layout->addLayout(form);
        layout->addWidget(m_error);
        layout->addWidget(buttons);
    }

    QString name() const
    {
        const QString typed = m_name->text().trimmed();
        return typed.isEmpty() ? m_normalized : typed;
    }
    QString apiRoot() const { return m_normalized; }
    QString token() const { return m_token->text().trimmed(); }

private:
    void validateAndAccept()
    {
        const auto result = ListenBrainzUrl::normalizeBase(m_url->text());
        if (!result.valid) {
            m_error->setText(result.error);
            m_error->show();
            m_url->setFocus();
            return;
        }
        m_normalized = result.apiRoot;
        accept();
    }

    QLineEdit *m_name = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_token = nullptr;
    QLabel *m_error = nullptr;
    QString m_normalized;
};

}   // namespace

ScrobbleDestinationsDialog::ScrobbleDestinationsDialog(ScrobbleDestinationSet destinations,
                                                       ListenHistoryStore *history, Callbacks callbacks,
                                                       QWidget *parent)
    : QDialog(parent)
    , m_destinations(std::move(destinations))
    , m_history(history)
    , m_callbacks(std::move(callbacks))
{
    setWindowTitle(QStringLiteral("Scrobblers"));
    resize(760, 380);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type"), QStringLiteral("URL"),
                                        QStringLiteral("Enabled"), QStringLiteral("Status"),
                                        QStringLiteral("Pending")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(UrlColumn, QHeaderView::Stretch);

    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->hide();

    auto *addButton = new QPushButton(QStringLiteral("Add server…"), this);
    m_editButton = new QPushButton(QStringLiteral("Edit…"), this);
    m_removeButton = new QPushButton(QStringLiteral("Remove"), this);
    m_testButton = new QPushButton(QStringLiteral("Test"), this);
    m_toggleButton = new QPushButton(QStringLiteral("Enable"), this);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        applyRemovals();
        for (auto it = m_pendingTokens.constBegin(); it != m_pendingTokens.constEnd(); ++it) {
            m_callbacks.writeToken(it.key(), it.value());
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(addButton, &QPushButton::clicked, this, &ScrobbleDestinationsDialog::addDestination);
    connect(m_editButton, &QPushButton::clicked, this, &ScrobbleDestinationsDialog::editSelected);
    connect(m_removeButton, &QPushButton::clicked, this, &ScrobbleDestinationsDialog::removeSelected);
    connect(m_testButton, &QPushButton::clicked, this, &ScrobbleDestinationsDialog::testSelected);
    connect(m_toggleButton, &QPushButton::clicked, this, &ScrobbleDestinationsDialog::toggleSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &ScrobbleDestinationsDialog::updateButtons);
    connect(m_table, &QTableWidget::itemDoubleClicked, this, &ScrobbleDestinationsDialog::editSelected);

    auto *actions = new QHBoxLayout;
    actions->addWidget(addButton);
    actions->addWidget(m_editButton);
    actions->addWidget(m_removeButton);
    actions->addWidget(m_testButton);
    actions->addWidget(m_toggleButton);
    actions->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_table);
    layout->addWidget(m_warning);
    layout->addLayout(actions);
    layout->addWidget(buttons);

    reloadTable();
}

void ScrobbleDestinationsDialog::reloadTable()
{
    const QString keepSelected = selectedId();

    m_table->setRowCount(static_cast<int>(m_destinations.items.size()));
    int row = 0;
    for (const ScrobbleDestination &destination : m_destinations.items) {
        const bool compatible = destination.type == ScrobbleDestination::Type::ListenBrainzCompatible;

        QString status = m_statusById.value(destination.id);
        if (status.isEmpty()) {
            if (compatible) {
                const QString token = m_pendingTokens.value(destination.id, m_callbacks.readToken(destination.id));
                status = token.isEmpty() ? QStringLiteral("No token") : QStringLiteral("Token set");
            } else {
                status = m_callbacks.lastFmConfigured() ? QStringLiteral("Signed in")
                                                        : QStringLiteral("Not signed in");
            }
        }

        const int pending = m_history != nullptr ? m_history->pendingCount(destination.id) : 0;

        const QStringList cells = {
            destination.name,
            compatible ? QStringLiteral("ListenBrainz") : QStringLiteral("Last.fm"),
            compatible ? destination.apiRoot : kEmptyCell,
            destination.enabled ? QStringLiteral("Yes") : QStringLiteral("No"),
            status,
            pending > 0 ? QString::number(pending) : kEmptyCell,
        };
        for (int column = 0; column < ColumnCount; ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            if (column == NameColumn) {
                item->setData(Qt::UserRole, destination.id);
            }
            m_table->setItem(row, column, item);
        }
        ++row;
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(UrlColumn, QHeaderView::Stretch);

    for (int index = 0; index < m_table->rowCount(); ++index) {
        if (m_table->item(index, NameColumn)->data(Qt::UserRole).toString() == keepSelected) {
            m_table->selectRow(index);
            break;
        }
    }
    if (m_table->currentRow() < 0 && m_table->rowCount() > 0) {
        m_table->selectRow(0);
    }

    updateDuplicateRelayWarning();
    updateButtons();
}

void ScrobbleDestinationsDialog::updateDuplicateRelayWarning()
{
    int enabledCompatible = 0;
    for (const ScrobbleDestination &destination : m_destinations.items) {
        if (destination.enabled && destination.type == ScrobbleDestination::Type::ListenBrainzCompatible) {
            ++enabledCompatible;
        }
    }
    // Informational, never blocking: relaying to several servers is a
    // legitimate setup, it just means each listen is submitted more than once.
    m_warning->setVisible(enabledCompatible > 1);
    m_warning->setText(QStringLiteral("%1 ListenBrainz-compatible destinations are enabled. Every listen is "
                                      "submitted to each of them, so any server that relays to another may "
                                      "receive it twice.")
                           .arg(enabledCompatible));
}

QString ScrobbleDestinationsDialog::selectedId() const
{
    const int row = m_table->currentRow();
    if (row < 0 || m_table->item(row, NameColumn) == nullptr) {
        return {};
    }
    return m_table->item(row, NameColumn)->data(Qt::UserRole).toString();
}

void ScrobbleDestinationsDialog::updateButtons()
{
    const ScrobbleDestination *selected = m_destinations.find(selectedId());
    const bool compatible = selected != nullptr
        && selected->type == ScrobbleDestination::Type::ListenBrainzCompatible;

    // The reserved destinations are part of the app: their identity and URL are
    // not the user's to edit, and they cannot be removed.
    m_editButton->setEnabled(compatible && !selected->isReserved());
    m_removeButton->setEnabled(selected != nullptr && !selected->isReserved());
    m_testButton->setEnabled(compatible);
    m_toggleButton->setEnabled(selected != nullptr);
    m_toggleButton->setText(selected != nullptr && selected->enabled ? QStringLiteral("Disable")
                                                                    : QStringLiteral("Enable"));
}

void ScrobbleDestinationsDialog::addDestination()
{
    CompatibleDestinationDialog editor(QStringLiteral("Add scrobbling server"), {}, {}, {}, this);
    if (editor.exec() != QDialog::Accepted) {
        return;
    }

    const QString id = m_destinations.addCustom(editor.name(), editor.apiRoot(), true);
    m_pendingTokens.insert(id, editor.token());
    reloadTable();
}

void ScrobbleDestinationsDialog::editSelected()
{
    const QString id = selectedId();
    const ScrobbleDestination *existing = m_destinations.find(id);
    if (existing == nullptr || existing->isReserved()
        || existing->type != ScrobbleDestination::Type::ListenBrainzCompatible) {
        return;
    }

    const QString token = m_pendingTokens.value(id, m_callbacks.readToken(id));
    CompatibleDestinationDialog editor(QStringLiteral("Edit scrobbling server"), existing->name,
                                       existing->apiRoot, token, this);
    if (editor.exec() != QDialog::Accepted) {
        return;
    }

    const int pending = m_history != nullptr ? m_history->pendingCount(id) : 0;
    if (editor.apiRoot() != existing->apiRoot && pending > 0) {
        // The backlog belongs to the destination, not to the URL, so it follows
        // the destination to its new address. Say so before it happens.
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Change server URL"),
            QStringLiteral("\"%1\" has %2 listens waiting to be sent. They stay queued and will be "
                           "delivered to the new address instead. Continue?")
                .arg(existing->name)
                .arg(pending),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    for (ScrobbleDestination &destination : m_destinations.items) {
        if (destination.id == id) {
            destination.name = editor.name();
            destination.apiRoot = editor.apiRoot();
        }
    }
    m_pendingTokens.insert(id, editor.token());
    m_statusById.remove(id);
    reloadTable();
}

void ScrobbleDestinationsDialog::removeSelected()
{
    const QString id = selectedId();
    const ScrobbleDestination *existing = m_destinations.find(id);
    if (existing == nullptr || existing->isReserved()) {
        return;
    }

    const int pending = m_history != nullptr ? m_history->pendingCount(id) : 0;
    const QString detail = pending > 0
        ? QStringLiteral("\"%1\" has %2 listens waiting to be sent. Removing it discards them.")
              .arg(existing->name)
              .arg(pending)
        : QStringLiteral("Remove \"%1\"?").arg(existing->name);
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Remove scrobbler"),
        detail + QStringLiteral("\n\nYour local listening history is kept; only this destination's "
                                "delivery records and stored token are deleted."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    m_destinations.items.removeIf([&](const ScrobbleDestination &item) { return item.id == id; });
    m_pendingTokens.remove(id);
    m_statusById.remove(id);
    m_removedIds << id;
    reloadTable();
}

void ScrobbleDestinationsDialog::applyRemovals()
{
    for (const QString &id : std::as_const(m_removedIds)) {
        if (m_history != nullptr) {
            m_history->forgetDestination(id);
        }
        m_callbacks.removeToken(id);
    }
    m_removedIds.clear();
}

void ScrobbleDestinationsDialog::testSelected()
{
    const QString id = selectedId();
    const ScrobbleDestination *selected = m_destinations.find(id);
    if (selected == nullptr || selected->type != ScrobbleDestination::Type::ListenBrainzCompatible) {
        return;
    }

    m_statusById.insert(id, QStringLiteral("Testing…"));
    reloadTable();
    m_callbacks.testDestination(id, selected->apiRoot,
                               m_pendingTokens.value(id, m_callbacks.readToken(id)));
}

void ScrobbleDestinationsDialog::reportTestResult(const QString &destinationId, bool valid, const QString &username)
{
    if (m_destinations.find(destinationId) == nullptr) {
        return;
    }
    m_statusById.insert(destinationId,
                        valid ? QStringLiteral("Connected as %1").arg(username)
                              : QStringLiteral("Token rejected"));
    reloadTable();
}

void ScrobbleDestinationsDialog::toggleSelected()
{
    const QString id = selectedId();
    for (ScrobbleDestination &destination : m_destinations.items) {
        if (destination.id == id) {
            destination.enabled = !destination.enabled;
        }
    }
    reloadTable();
}
