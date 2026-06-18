#include "app/DemoScreens.h"

#include "app/AppCore.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPixmap>
#include <QThread>

namespace {

void waitForEvents(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

bool saveWindow(MainWindow &window, const QDir &dir, const QString &fileName, QString *error)
{
    waitForEvents(250);
    const QPixmap pixmap = window.grab();
    if (pixmap.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("grab for %1 returned a null pixmap").arg(fileName);
        }
        return false;
    }
    const QString path = dir.filePath(fileName);
    if (!pixmap.save(path, "PNG")) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to save %1").arg(path);
        }
        return false;
    }
    return true;
}

void activateDigitShortcut(MainWindow &window, int key)
{
    if (auto *edit = qobject_cast<QLineEdit *>(QApplication::focusWidget())) {
        edit->clearFocus();
    }
    window.setFocus(Qt::OtherFocusReason);

    const QString text = QString::number(key - Qt::Key_0);
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(&window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &release);
    waitForEvents(500);
}

void setFocusedSearchText(const QString &query)
{
    if (query.isEmpty()) {
        return;
    }
    if (auto *edit = qobject_cast<QLineEdit *>(QApplication::focusWidget())) {
        edit->setText(query);
        edit->setCursorPosition(static_cast<int>(query.size()));
        waitForEvents(1200);
    }
}

} // namespace

namespace DemoScreens {

bool capture(AppCore &core, const QString &outputDir, const QString &searchQuery, QString *error)
{
    MainWindow *window = core.window();
    if (window == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("demo capture requested before the main window exists");
        }
        return false;
    }

    QDir dir(outputDir);
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to create %1").arg(dir.absolutePath());
        }
        return false;
    }

    window->resize(1440, 900);
    window->show();
    waitForEvents(900);

    if (!saveWindow(*window, dir, QStringLiteral("01-library.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_4);
    setFocusedSearchText(searchQuery);
    if (!saveWindow(*window, dir, QStringLiteral("02-search.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_1);
    if (!saveWindow(*window, dir, QStringLiteral("03-queue.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_5);
    if (!saveWindow(*window, dir, QStringLiteral("04-playlists.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_3);
    if (!saveWindow(*window, dir, QStringLiteral("05-explorer.png"), error)) return false;

    return true;
}

} // namespace DemoScreens
