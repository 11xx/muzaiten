#include "app/DemoScreens.h"

#include "app/AppCore.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QEventLoop>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>

namespace {

void waitForEvents(int ms);

QPalette lightPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(246, 246, 246));
    palette.setColor(QPalette::WindowText, QColor(24, 24, 24));
    palette.setColor(QPalette::Base, QColor(255, 255, 255));
    palette.setColor(QPalette::AlternateBase, QColor(242, 242, 242));
    palette.setColor(QPalette::Text, QColor(24, 24, 24));
    palette.setColor(QPalette::Button, QColor(240, 240, 240));
    palette.setColor(QPalette::ButtonText, QColor(24, 24, 24));
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, QColor(35, 111, 190));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor(255, 255, 220));
    palette.setColor(QPalette::ToolTipText, QColor(24, 24, 24));
    return palette;
}

QPalette darkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(35, 35, 38));
    palette.setColor(QPalette::WindowText, QColor(232, 232, 232));
    palette.setColor(QPalette::Base, QColor(27, 27, 30));
    palette.setColor(QPalette::AlternateBase, QColor(42, 42, 46));
    palette.setColor(QPalette::Text, QColor(232, 232, 232));
    palette.setColor(QPalette::Button, QColor(45, 45, 49));
    palette.setColor(QPalette::ButtonText, QColor(232, 232, 232));
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, QColor(73, 145, 217));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor(45, 45, 49));
    palette.setColor(QPalette::ToolTipText, QColor(232, 232, 232));
    palette.setColor(QPalette::PlaceholderText, QColor(170, 170, 170));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(128, 128, 128));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(128, 128, 128));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(128, 128, 128));
    return palette;
}

QString themeDirectoryName(const QString &theme)
{
    QString name = theme.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:]")), QStringLiteral("_"));
    return name.isEmpty() ? QStringLiteral("default") : name;
}

QString currentPlasmaColorScheme()
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("plasma-apply-colorscheme"));
    if (tool.isEmpty()) {
        return {};
    }
    QProcess process;
    process.start(tool, {QStringLiteral("--list-schemes")});
    if (!process.waitForFinished(3000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return {};
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QRegularExpression currentRe(QStringLiteral("^\\s*\\*\\s+(.+?)\\s+\\(current color scheme\\)\\s*$"),
                                       QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = currentRe.match(output);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

bool applyPlasmaColorScheme(const QString &scheme, QString *error)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("plasma-apply-colorscheme"));
    if (tool.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("plasma-apply-colorscheme not found; cannot apply demo theme %1").arg(scheme);
        }
        return false;
    }
    QProcess process;
    process.start(tool, {QStringLiteral("--platform"), QStringLiteral("offscreen"), scheme});
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error != nullptr) {
            const QString details = QString::fromUtf8(process.readAllStandardError()).trimmed();
            *error = QStringLiteral("failed to apply demo theme %1 with plasma-apply-colorscheme (exit %2)%3")
                         .arg(scheme)
                         .arg(process.exitCode())
                         .arg(details.isEmpty() ? QString() : QStringLiteral(": %1").arg(details));
        }
        return false;
    }
    waitForEvents(350);
    return true;
}

bool applyColorScheme(const QString &scheme, QString *error)
{
    const QString normalized = scheme.trimmed().toLower();
    if (normalized == QStringLiteral("dark")) {
        qApp->setPalette(darkPalette());
    } else if (normalized == QStringLiteral("light")) {
        qApp->setPalette(lightPalette());
    } else {
        return applyPlasmaColorScheme(scheme, error);
    }
    waitForEvents(150);
    return true;
}

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

bool writeSearchVideo(MainWindow &window,
                      const QDir &dir,
                      const QString &query,
                      int keyDelayMs,
                      const QString &finalStillPath,
                      QString *error)
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
    QString lastFramePath;
    const auto saveFrame = [&]() {
        lastFramePath = frameDir.filePath(QStringLiteral("search_%1.png").arg(frame++, 5, 10, QLatin1Char('0')));
        return saveWindow(window, lastFramePath, error);
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

    if (!finalStillPath.isEmpty()) {
        QFile::remove(finalStillPath);
        if (!QFile::copy(lastFramePath, finalStillPath)) {
            if (error != nullptr) {
                *error = QStringLiteral("failed to save final search still %1").arg(finalStillPath);
            }
            return false;
        }
    }

    frameDir.removeRecursively();
    return true;
}

QStringList normalizedSchemes(const QStringList &schemes)
{
    QStringList result;
    for (const QString &scheme : schemes) {
        const QString trimmed = scheme.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QString normalized = trimmed.toLower();
        if (normalized == QStringLiteral("both")) {
            result << QStringLiteral("light") << QStringLiteral("dark");
        } else {
            result << trimmed;
        }
    }
    result.removeDuplicates();
    return result;
}

bool captureOne(AppCore &core, const DemoScreens::Options &options, const QDir &dir, QString *error)
{
    MainWindow *window = core.window();
    if (window == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("demo capture requested before the main window exists");
        }
        return false;
    }

    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to create %1").arg(dir.absolutePath());
        }
        return false;
    }

    window->resize(options.windowSize);
    window->show();
    waitForEvents(900);

    if (!options.nowPlayingQuery.trimmed().isEmpty()
        && !window->showDemoNowPlaying(options.nowPlayingQuery,
                                       options.nowPlaying,
                                       options.nowPlayingPositionRatio,
                                       error)) {
        return false;
    }

    if (!options.albumTitle.trimmed().isEmpty()) {
        if (!window->showDemoAlbum(options.artistName, options.albumTitle, error)) {
            return false;
        }
    } else if (!options.artistName.trimmed().isEmpty() && !window->showDemoArtist(options.artistName)) {
        if (error != nullptr) {
            *error = QStringLiteral("artist not found for demo capture: %1").arg(options.artistName);
        }
        return false;
    }
    waitForEvents(1800);
    if (!saveWindow(*window, dir, QStringLiteral("01-library.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_4);
    if (options.searchVideo) {
        if (!writeSearchVideo(*window,
                              dir,
                              options.searchQuery,
                              options.searchKeyDelayMs,
                              dir.filePath(QStringLiteral("02-search.png")),
                              error)) {
            return false;
        }
    } else {
        setFocusedSearchText(options.searchQuery);
        if (!saveWindow(*window, dir, QStringLiteral("02-search.png"), error)) return false;
    }

    activateDigitShortcut(*window, Qt::Key_1);
    if (!saveWindow(*window, dir, QStringLiteral("03-queue.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_5);
    if (!saveWindow(*window, dir, QStringLiteral("04-playlists.png"), error)) return false;

    activateDigitShortcut(*window, Qt::Key_3);
    if (!saveWindow(*window, dir, QStringLiteral("05-explorer.png"), error)) return false;

    return true;
}

} // namespace

namespace DemoScreens {

bool capture(AppCore &core, const Options &options, QString *error)
{
    QDir dir(options.outputDir);
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to create %1").arg(dir.absolutePath());
        }
        return false;
    }

    const QStringList schemes = normalizedSchemes(options.colorSchemes);
    if (schemes.isEmpty()) {
        return captureOne(core, options, dir, error);
    }
    const QString restoreScheme = currentPlasmaColorScheme();
    for (const QString &scheme : schemes) {
        if (!applyColorScheme(scheme, error)) {
            return false;
        }
        QDir schemeDir(dir.filePath(themeDirectoryName(scheme)));
        if (!captureOne(core, options, schemeDir, error)) {
            if (!restoreScheme.isEmpty()) {
                QString ignored;
                applyPlasmaColorScheme(restoreScheme, &ignored);
            }
            return false;
        }
    }
    if (!restoreScheme.isEmpty()) {
        QString ignored;
        applyPlasmaColorScheme(restoreScheme, &ignored);
    }

    return true;
}

} // namespace DemoScreens
