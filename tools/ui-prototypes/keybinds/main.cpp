// Throwaway prototype of the Keybinds editor. Fake data only.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

struct Row {
    QString action;
    QStringList keys;
    bool custom = false;
};

struct Group {
    QString title;
    QVector<Row> rows;
};

QVector<Group> queueGroups()
{
    return {
        {QStringLiteral("Navigation"),
         {{QStringLiteral("Move down"), {QStringLiteral("J"), QStringLiteral("N"), QStringLiteral("Down"), QStringLiteral("Ctrl+N")}},
          {QStringLiteral("Move up"), {QStringLiteral("K"), QStringLiteral("P"), QStringLiteral("Up"), QStringLiteral("Ctrl+P")}},
          {QStringLiteral("Page down"), {QStringLiteral("PgDown"), QStringLiteral("Ctrl+V")}},
          {QStringLiteral("Page up"), {QStringLiteral("PgUp"), QStringLiteral("Alt+V")}},
          {QStringLiteral("Jump to the playing track"), {QStringLiteral("O")}}}},
        {QStringLiteral("Playback"),
         {{QStringLiteral("Play selected"), {QStringLiteral("Return"), QStringLiteral("Enter"), QStringLiteral("Space")}}}},
        {QStringLiteral("Editing the queue"),
         {{QStringLiteral("Move item down"), {QStringLiteral("Shift+N"), QStringLiteral("Shift+J")}, true},
          {QStringLiteral("Move item up"), {QStringLiteral("Shift+P")}},
          {QStringLiteral("Remove selected"), {QStringLiteral("D"), QStringLiteral("Del")}},
          {QStringLiteral("Clear queue"), {QStringLiteral("C")}},
          {QStringLiteral("Clear play-next priority"), {QStringLiteral("X")}},
          {QStringLiteral("Add to playlist"), {}, true}}},
        {QStringLiteral("Search"),
         {{QStringLiteral("Search the queue"), {QStringLiteral("/"), QStringLiteral("Ctrl+S")}},
          {QStringLiteral("Next match"), {QStringLiteral("Alt+N")}},
          {QStringLiteral("Previous match"), {QStringLiteral("Alt+P")}},
          {QStringLiteral("Close search"), {QStringLiteral("Esc"), QStringLiteral("Ctrl+G")}}}},
        {QStringLiteral("Find elsewhere"),
         {{QStringLiteral("Find in library"), {QStringLiteral("L")}},
          {QStringLiteral("Find the file"), {QStringLiteral("F")}},
          {QStringLiteral("Show in file explorer"), {}}}},
    };
}

QTreeWidget *bindingTree(QWidget *parent, const QVector<Group> &groups, QTreeWidgetItem **selectedOut)
{
    auto *tree = new QTreeWidget(parent);
    tree->setColumnCount(2);
    tree->setHeaderLabels({QStringLiteral("Action"), QStringLiteral("Keys")});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    const QColor muted = tree->palette().color(QPalette::PlaceholderText);
    for (const Group &group : groups) {
        auto *heading = new QTreeWidgetItem(tree, {group.title});
        QFont font = heading->font(0);
        font.setBold(true);
        heading->setFont(0, font);
        heading->setFirstColumnSpanned(true);
        heading->setFlags(Qt::ItemIsEnabled);
        heading->setExpanded(true);
        for (const Row &row : group.rows) {
            auto *item = new QTreeWidgetItem(heading, {row.action, row.keys.isEmpty() ? QStringLiteral("Not assigned") : row.keys.join(QStringLiteral(", "))});
            if (row.keys.isEmpty()) {
                item->setForeground(1, muted);
            }
            if (row.custom) {
                QFont bold = item->font(1);
                bold.setBold(true);
                item->setFont(1, bold);
                item->setToolTip(1, QStringLiteral("Changed from the preset"));
            }
            if (row.action == QStringLiteral("Move item down") && selectedOut != nullptr) {
                *selectedOut = item;
            }
        }
    }
    tree->expandAll();
    return tree;
}

QWidget *keyChip(QWidget *parent, const QString &key)
{
    auto *chip = new QToolButton(parent);
    chip->setText(key);
    chip->setIcon(parent->style()->standardIcon(QStyle::SP_LineEditClearButton));
    chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    chip->setToolTip(QStringLiteral("Remove %1").arg(key));
    chip->setLayoutDirection(Qt::RightToLeft);
    return chip;
}

QWidget *editablePage(QWidget *parent, const QString &title, const QVector<Group> &groups)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *titleRow = new QHBoxLayout;
    auto *heading = new QLabel(title, page);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.25);
    heading->setFont(headingFont);
    titleRow->addWidget(heading);
    titleRow->addStretch(1);
    titleRow->addWidget(new QLabel(QStringLiteral("Preset"), page));
    auto *preset = new QComboBox(page);
    preset->addItem(QStringLiteral("dired + hjkl"));
    titleRow->addWidget(preset);
    layout->addLayout(titleRow);

    auto *searchRow = new QHBoxLayout;
    auto *filter = new QLineEdit(page);
    filter->setPlaceholderText(QStringLiteral("Filter actions"));
    filter->setClearButtonEnabled(true);
    searchRow->addWidget(filter, 1);
    auto *find = new QPushButton(QStringLiteral("Find a key…"), page);
    find->setToolTip(QStringLiteral("Press a key to see what it does on this screen"));
    searchRow->addWidget(find);
    layout->addLayout(searchRow);

    QTreeWidgetItem *selected = nullptr;
    auto *tree = bindingTree(page, groups, &selected);
    layout->addWidget(tree, 1);
    if (selected != nullptr) {
        tree->setCurrentItem(selected);
    }

    auto *editor = new QWidget(page);
    auto *editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(0, 4, 0, 0);
    auto *actionName = new QLabel(QStringLiteral("Move item down"), editor);
    QFont actionFont = actionName->font();
    actionFont.setBold(true);
    actionName->setFont(actionFont);
    editorLayout->addWidget(actionName);
    auto *keysRow = new QHBoxLayout;
    keysRow->addWidget(keyChip(editor, QStringLiteral("Shift+N")));
    keysRow->addWidget(keyChip(editor, QStringLiteral("Shift+J")));
    auto *capture = new QKeySequenceEdit(QKeySequence(QStringLiteral("Alt+N")), editor);
    capture->setMaximumSequenceLength(1);
    keysRow->addWidget(capture);
    keysRow->addStretch(1);
    keysRow->addWidget(new QPushButton(QStringLiteral("Reset to preset"), editor));
    editorLayout->addLayout(keysRow);
    auto *conflictRow = new QHBoxLayout;
    auto *conflict = new QLabel(QStringLiteral("Alt+N is assigned to Next match. Moving it here unassigns it there."), editor);
    conflict->setWordWrap(true);
    conflictRow->addWidget(conflict, 1);
    conflictRow->addWidget(new QPushButton(QStringLiteral("Move key here"), editor));
    conflictRow->addWidget(new QPushButton(QStringLiteral("Cancel"), editor));
    editorLayout->addLayout(conflictRow);
    layout->addWidget(editor);
    return page;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Keybinds"));
    dialog.resize(820, 600);
    auto *root = new QVBoxLayout(&dialog);
    auto *body = new QHBoxLayout;
    auto *screens = new QListWidget(&dialog);
    for (const QString &name : {QStringLiteral("Queue"), QStringLiteral("Library"), QStringLiteral("File explorer"),
                                QStringLiteral("Search"), QStringLiteral("Playlists"), QStringLiteral("Global")}) {
        screens->addItem(name);
    }
    screens->setCurrentRow(0);
    screens->setFixedWidth(150);
    body->addWidget(screens);
    auto *pages = new QStackedWidget(&dialog);
    pages->addWidget(editablePage(pages, QStringLiteral("Queue"), queueGroups()));
    body->addWidget(pages, 1);
    root->addLayout(body, 1);

    auto *footer = new QHBoxLayout;
    footer->addWidget(new QPushButton(QStringLiteral("Restore the Queue defaults"), &dialog));
    footer->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    footer->addWidget(buttons);
    root->addLayout(footer);

    const QString shotDir = qEnvironmentVariable("PROTOTYPE_SHOT_DIR");
    dialog.show();
    if (!shotDir.isEmpty()) {
        QTimer::singleShot(600, &app, [&dialog, shotDir] {
            dialog.grab().save(shotDir + QStringLiteral("/keybinds.png"));
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
