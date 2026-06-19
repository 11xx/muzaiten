#include "app/MuzaitenApplication.h"

#include "Version.h"
#include "app/AppCore.h"
#include "app/DemoScreens.h"
#include "core/Track.h"
#include "ipc/IpcSocket.h"
#include "scanner/TagReader.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTimer>

#include <taglib/tdebuglistener.h>

namespace {

class TagLibQtDebugListener final : public TagLib::DebugListener {
public:
    explicit TagLibQtDebugListener(bool verbose)
        : m_verbose(verbose)
    {
    }

    void printMessage(const TagLib::String &msg) override
    {
        if (!m_verbose) {
            return;
        }
        // TagLib calls this synchronously on the thread doing the read, so the
        // thread-local current file attributes each warning to its source.
        QString line = QStringLiteral("TagLib: %1").arg(QString::fromStdString(msg.to8Bit(true)).trimmed());
        const QString file = TagReader::currentScanFile();
        if (!file.isEmpty()) {
            line += QStringLiteral("  [while reading %1]").arg(file);
        }
        qWarning().noquote() << line;
    }

private:
    bool m_verbose = false;
};

QSize parseDemoSize(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    const QStringList parts = normalized.split(QLatin1Char('x'));
    if (parts.size() != 2) {
        return QSize(1440, 900);
    }
    bool widthOk = false;
    bool heightOk = false;
    const int width = parts.at(0).toInt(&widthOk);
    const int height = parts.at(1).toInt(&heightOk);
    if (!widthOk || !heightOk || width < 800 || height < 500) {
        return QSize(1440, 900);
    }
    return QSize(width, height);
}

} // namespace

MuzaitenApplication::MuzaitenApplication(int &argc, char **argv)
    : QApplication(argc, argv)
{
    QCoreApplication::setApplicationName(QStringLiteral(MUZAITEN_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(MUZAITEN_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("11xx"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("11xx.org"));
    const QString desktopFile = QStringLiteral(MUZAITEN_APP_ID ".desktop");
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopFile).isEmpty()) {
        setDesktopFileName(QStringLiteral(MUZAITEN_APP_ID));
    }

    setWindowIcon(QIcon::fromTheme(QStringLiteral(MUZAITEN_APP_ID),
                                   QIcon(QStringLiteral(":/icons/muzaiten.svg"))));

    qRegisterMetaType<Track>("Track");
    qRegisterMetaType<QVector<Track>>("QVector<Track>");
    configureCommandLine();
}

// Single-instance check, scoped to the resolved state dir: instances with
// different state roots (dev-state, agent-state, ...) use different sockets
// and stay fully isolated, but launching a second copy against the same state
// just raises the running one's window — two processes sharing one set of
// sqlite stores would corrupt each other's view of the world.
bool MuzaitenApplication::raiseRunningInstance()
{
    QLocalSocket socket;
    socket.connectToServer(IpcSocket::serverPath());
    if (!socket.waitForConnected(500)) {
        return false;  // no live instance (stale sockets refuse the connect)
    }
    socket.write(QJsonDocument(QJsonObject{{QStringLiteral("command"), QStringLiteral("raise")}})
                     .toJson(QJsonDocument::Compact) + '\n');
    socket.waitForReadyRead(2000);
    return true;
}

int MuzaitenApplication::run()
{
    const QString demoScreensDir = property("muzaiten.demoScreensDir").toString().trimmed();
    if (!demoScreensDir.isEmpty()) {
        const bool isolatedState = !property("muzaiten.stateRoot").toString().trimmed().isEmpty()
            || property("muzaiten.devState").toBool()
            || qEnvironmentVariableIsSet("MUZAITEN_STATE_ROOT")
            || qEnvironmentVariableIsSet("MUZAITEN_DEV_STATE");
        if (!isolatedState) {
            qCritical("refusing --demo-screens without --state-root, --dev-state, MUZAITEN_STATE_ROOT, or MUZAITEN_DEV_STATE");
            return 2;
        }

        AppCore core;
        core.showWindow();
        QTimer::singleShot(0, this, [this, &core, demoScreensDir]() {
            QString error;
            DemoScreens::Options options;
            options.outputDir = demoScreensDir;
            options.searchQuery = property("muzaiten.demoSearch").toString();
            options.artistName = property("muzaiten.demoArtist").toString();
            options.albumTitle = property("muzaiten.demoAlbum").toString();
            options.nowPlayingQuery = property("muzaiten.demoNowPlaying").toString();
            options.colorSchemes = property("muzaiten.demoThemes").toStringList();
            options.windowSize = property("muzaiten.demoSize").toSize();
            options.nowPlaying = property("muzaiten.demoNowPlayingState").toString() == QStringLiteral("playing");
            options.searchVideo = property("muzaiten.demoSearchVideo").toBool();
            options.searchKeyDelayMs = property("muzaiten.demoSearchDelayMs").toInt();
            options.nowPlayingPositionRatio = property("muzaiten.demoNowPlayingPosition").toDouble();
            const bool ok = DemoScreens::capture(core, options, &error);
            if (!ok) {
                qCritical().noquote() << error;
            }
            setProperty("muzaiten.demoExitCode", ok ? 0 : 1);
            core.quit();
        });
        exec();
        return property("muzaiten.demoExitCode").toInt();
    }

    if (raiseRunningInstance()) {
        qInfo("muzaiten is already running against this state root; raised its window instead.");
        return 0;
    }
    AppCore core;
    core.showWindow();
    return exec();
}

void MuzaitenApplication::configureCommandLine()
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native Linux music player"));
    parser.addHelpOption();
    parser.addVersionOption();

    QStringList verboseNames;
    verboseNames << QStringLiteral("verbose");
    const QCommandLineOption verboseOption(verboseNames, QStringLiteral("Enable verbose debug logging."));
    // Path overrides. Default is XDG; these provide explicit, full customization.
    const QCommandLineOption stateRootOption(QStringLiteral("state-root"), QStringLiteral("Store all data/state/cache/config under <path>/{data,state,cache,config}."), QStringLiteral("path"));
    // Dev-only shortcut; functional but kept out of --help (HiddenFromHelp).
    QCommandLineOption devStateOption(QStringLiteral("dev-state"), QStringLiteral("Shortcut for --state-root ./dev-state (relative to the current directory)."));
    devStateOption.setFlags(QCommandLineOption::HiddenFromHelp);
    const QCommandLineOption dataDirOption(QStringLiteral("data-dir"), QStringLiteral("Override the data directory (library database)."), QStringLiteral("path"));
    const QCommandLineOption stateDirOption(QStringLiteral("state-dir"), QStringLiteral("Override the state directory (UI prefs, session, pending scrobbles)."), QStringLiteral("path"));
    const QCommandLineOption cacheDirOption(QStringLiteral("cache-dir"), QStringLiteral("Override the cache directory (artwork)."), QStringLiteral("path"));
    const QCommandLineOption configDirOption(QStringLiteral("config-dir"), QStringLiteral("Override the config directory (holds muzaiten.conf)."), QStringLiteral("path"));
    QCommandLineOption demoScreensOption(QStringLiteral("demo-screens"), QStringLiteral("Hidden: capture publishing screenshots into <dir> and exit."), QStringLiteral("dir"));
    demoScreensOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoSearchOption(QStringLiteral("demo-search"), QStringLiteral("Hidden: query to type before the search screenshot."), QStringLiteral("query"));
    demoSearchOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoSearchVideoOption(QStringLiteral("demo-search-video"), QStringLiteral("Hidden: record typed search frames to 02-search.mp4."));
    demoSearchVideoOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoSearchDelayOption(QStringLiteral("demo-search-delay-ms"), QStringLiteral("Hidden: per-key search video delay in milliseconds."), QStringLiteral("ms"));
    demoSearchDelayOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoArtistOption(QStringLiteral("demo-artist"), QStringLiteral("Hidden: select an album artist before the library screenshot."), QStringLiteral("name"));
    demoArtistOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoAlbumOption(QStringLiteral("demo-album"), QStringLiteral("Hidden: highlight an album before the library screenshot."), QStringLiteral("title"));
    demoAlbumOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoThemeOption(QStringLiteral("demo-theme"), QStringLiteral("Hidden: capture a color scheme: light, dark, or both. Repeatable."), QStringLiteral("theme"));
    demoThemeOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoSizeOption(QStringLiteral("demo-size"), QStringLiteral("Hidden: screenshot window size, for example 1440x900."), QStringLiteral("size"));
    demoSizeOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoNowPlayingOption(QStringLiteral("demo-now-playing"), QStringLiteral("Hidden: present the first track matching <query> in the player bar."), QStringLiteral("query"));
    demoNowPlayingOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoNowPlayingStateOption(QStringLiteral("demo-now-playing-state"), QStringLiteral("Hidden: player-bar visual state, playing or paused."), QStringLiteral("state"));
    demoNowPlayingStateOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption demoNowPlayingPositionOption(QStringLiteral("demo-now-playing-position"), QStringLiteral("Hidden: player-bar progress ratio, 0.0 to 1.0."), QStringLiteral("ratio"));
    demoNowPlayingPositionOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(verboseOption);
    parser.addOption(stateRootOption);
    parser.addOption(devStateOption);
    parser.addOption(dataDirOption);
    parser.addOption(stateDirOption);
    parser.addOption(cacheDirOption);
    parser.addOption(configDirOption);
    parser.addOption(demoScreensOption);
    parser.addOption(demoSearchOption);
    parser.addOption(demoSearchVideoOption);
    parser.addOption(demoSearchDelayOption);
    parser.addOption(demoArtistOption);
    parser.addOption(demoAlbumOption);
    parser.addOption(demoThemeOption);
    parser.addOption(demoSizeOption);
    parser.addOption(demoNowPlayingOption);
    parser.addOption(demoNowPlayingStateOption);
    parser.addOption(demoNowPlayingPositionOption);
    parser.process(*this);

    const bool verbose = parser.isSet(verboseOption) || qEnvironmentVariableIsSet("MUZAITEN_VERBOSE");
    configureLogging(verbose);
    setProperty("muzaiten.verbose", verbose);

    // Store CLI-flag-derived overrides as properties; AppPaths applies the full
    // precedence (flag > env > combined root > XDG default) and reads env vars.
    const QString cliRoot = parser.value(stateRootOption).trimmed();
    if (!cliRoot.isEmpty()) {
        setProperty("muzaiten.stateRoot", QDir(cliRoot).absolutePath());
    }
    setProperty("muzaiten.devState", parser.isSet(devStateOption));
    const auto setDirProperty = [this, &parser](const QCommandLineOption &option, const char *property) {
        const QString value = parser.value(option).trimmed();
        if (!value.isEmpty()) {
            setProperty(property, QDir(value).absolutePath());
        }
    };
    setDirProperty(dataDirOption, "muzaiten.dataDir");
    setDirProperty(stateDirOption, "muzaiten.stateDir");
    setDirProperty(cacheDirOption, "muzaiten.cacheDir");
    setDirProperty(configDirOption, "muzaiten.configDir");

    const QString demoScreensDir = parser.value(demoScreensOption).trimmed();
    if (!demoScreensDir.isEmpty()) {
        setProperty("muzaiten.demoScreensDir", QDir(demoScreensDir).absolutePath());
    }
    const QString demoSearch = parser.value(demoSearchOption).trimmed();
    if (!demoSearch.isEmpty()) {
        setProperty("muzaiten.demoSearch", demoSearch);
    }
    setProperty("muzaiten.demoSearchVideo", parser.isSet(demoSearchVideoOption));
    bool delayOk = false;
    const int searchDelay = parser.value(demoSearchDelayOption).toInt(&delayOk);
    setProperty("muzaiten.demoSearchDelayMs", delayOk ? searchDelay : 120);
    const QString demoArtist = parser.value(demoArtistOption).trimmed();
    if (!demoArtist.isEmpty()) {
        setProperty("muzaiten.demoArtist", demoArtist);
    }
    const QString demoAlbum = parser.value(demoAlbumOption).trimmed();
    if (!demoAlbum.isEmpty()) {
        setProperty("muzaiten.demoAlbum", demoAlbum);
    }
    const QStringList demoThemes = parser.values(demoThemeOption);
    if (!demoThemes.isEmpty()) {
        setProperty("muzaiten.demoThemes", demoThemes);
    }
    setProperty("muzaiten.demoSize", parseDemoSize(parser.value(demoSizeOption)));
    const QString demoNowPlaying = parser.value(demoNowPlayingOption).trimmed();
    if (!demoNowPlaying.isEmpty()) {
        setProperty("muzaiten.demoNowPlaying", demoNowPlaying);
    }
    const QString demoNowPlayingState = parser.value(demoNowPlayingStateOption).trimmed().toLower();
    setProperty("muzaiten.demoNowPlayingState",
                demoNowPlayingState == QStringLiteral("playing") ? QStringLiteral("playing") : QStringLiteral("paused"));
    bool positionOk = false;
    const double nowPlayingPosition = parser.value(demoNowPlayingPositionOption).toDouble(&positionOk);
    setProperty("muzaiten.demoNowPlayingPosition", positionOk ? nowPlayingPosition : (2.0 / 3.0));
}

void MuzaitenApplication::configureLogging(bool verbose)
{
    if (verbose) {
        QLoggingCategory::setFilterRules(QStringLiteral("muzaiten.*.debug=true\nmuzaiten.*.info=true"));
    } else {
        QLoggingCategory::setFilterRules(QStringLiteral("muzaiten.*.debug=false\nmuzaiten.*.info=false"));
    }

    static auto *tagLibListener = new TagLibQtDebugListener(verbose);
    TagLib::setDebugListener(tagLibListener);
}
