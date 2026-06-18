#include "app/DemoScreens.h"

#include "app/AppCore.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>

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

bool saveWindow(MainWindow &window, const QString &path, QString *error)
{
    waitForEvents(250);
    const QPixmap pixmap = window.grab();
    if (pixmap.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("grab for %1 returned a null pixmap").arg(QFileInfo(path).fileName());
        }
        return false;
    }
    if (!pixmap.save(path, "PNG")) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to save %1").arg(path);
        }
        return false;
    }
    return true;
}

bool saveWindow(MainWindow &window, const QDir &dir, const QString &fileName, QString *error)
{
    return saveWindow(window, dir.filePath(fileName), error);
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

bool writeSearchVideo(MainWindow &window, const QDir &dir, const QString &query, int keyDelayMs, QString *error)
{
    if (query.isEmpty()) {
        return true;
    }

    QDir frameDir(dir.filePath(QStringLiteral("search-video-frames")));
    if (frameDir.exists() && !frameDir.removeRecursively()) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to clear %1").arg(frameDir.absolutePath());
        }
        return false;
    }
    if (!QDir().mkpath(frameDir.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to create %1").arg(frameDir.absolutePath());
        }
        return false;
    }

    auto *edit = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    if (edit == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("search box is not focused");
        }
        return false;
    }

    const int delayMs = std::clamp(keyDelayMs, 40, 1000);
    int frame = 0;
    const auto saveFrame = [&]() {
        return saveWindow(window, frameDir.filePath(QStringLiteral("search_%1.png").arg(frame++, 5, 10, QLatin1Char('0'))), error);
    };

    edit->clear();
    waitForEvents(delayMs);
    if (!saveFrame()) return false;

    QString typed;
    for (const QChar ch : query) {
        typed.append(ch);
        edit->setText(typed);
        edit->setCursorPosition(static_cast<int>(typed.size()));
        waitForEvents(delayMs);
        if (!saveFrame()) return false;
    }

    const int holdFrames = std::max(6, 1200 / delayMs);
    for (int i = 0; i < holdFrames; ++i) {
        waitForEvents(delayMs);
        if (!saveFrame()) return false;
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("ffmpeg not found; search frames are in %1").arg(frameDir.absolutePath());
        }
        return false;
    }

    const double fps = 1000.0 / static_cast<double>(delayMs);
    const QString output = dir.filePath(QStringLiteral("02-search.mp4"));
    const QStringList args{
        QStringLiteral("-y"),
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-framerate"), QString::number(fps, 'f', 2),
        QStringLiteral("-i"), frameDir.filePath(QStringLiteral("search_%05d.png")),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-movflags"), QStringLiteral("+faststart"),
        output,
    };
    const int status = QProcess::execute(ffmpeg, args);
    if (status != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("ffmpeg failed with exit code %1; search frames are in %2")
                         .arg(status)
                         .arg(frameDir.absolutePath());
        }
        return false;
    }

    frameDir.removeRecursively();
    return true;
}

} // namespace

namespace DemoScreens {

bool capture(AppCore &core, const Options &options, QString *error)
{
    MainWindow *window = core.window();
    if (window == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("demo capture requested before the main window exists");
        }
        return false;
    }

    QDir dir(options.outputDir);
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to create %1").arg(dir.absolutePath());
        }
        return false;
    }

    window->resize(1440, 900);
    window->show();
    waitForEvents(900);

    if (!options.artistName.trimmed().isEmpty() && !window->showDemoArtist(options.artistName)) {
        if (error != nullptr) {
            *error = QStringLiteral("artist not found for demo capture: %1").arg(options.artistName);
        }
        return false;
    }
    if (!saveWindow(*window, dir, QStringLiteral("01-library.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_4);
    if (options.searchVideo) {
        if (!writeSearchVideo(*window, dir, options.searchQuery, options.searchKeyDelayMs, error)) return false;
    } else {
        setFocusedSearchText(options.searchQuery);
    }
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
