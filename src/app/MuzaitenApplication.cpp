#include "app/MuzaitenApplication.h"

#include "Version.h"
#include "core/Track.h"
#include "ui/MainWindow.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include <QStandardPaths>

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
        if (m_verbose) {
            qWarning().noquote() << "TagLib:" << QString::fromStdString(msg.to8Bit(true)).trimmed();
        }
    }

private:
    bool m_verbose = false;
};

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

    qRegisterMetaType<Track>("Track");
    qRegisterMetaType<QVector<Track>>("QVector<Track>");
    configureCommandLine();
}

int MuzaitenApplication::run()
{
    MainWindow window;
    window.show();
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
    const QCommandLineOption devStateOption(QStringLiteral("dev-state"), QStringLiteral("Shortcut for --state-root ./dev-state (relative to the current directory)."));
    const QCommandLineOption dataDirOption(QStringLiteral("data-dir"), QStringLiteral("Override the data directory (library database)."), QStringLiteral("path"));
    const QCommandLineOption stateDirOption(QStringLiteral("state-dir"), QStringLiteral("Override the state directory (UI prefs, session, pending scrobbles)."), QStringLiteral("path"));
    const QCommandLineOption cacheDirOption(QStringLiteral("cache-dir"), QStringLiteral("Override the cache directory (artwork)."), QStringLiteral("path"));
    const QCommandLineOption configDirOption(QStringLiteral("config-dir"), QStringLiteral("Override the config directory (holds muzaiten.conf)."), QStringLiteral("path"));
    parser.addOption(verboseOption);
    parser.addOption(stateRootOption);
    parser.addOption(devStateOption);
    parser.addOption(dataDirOption);
    parser.addOption(stateDirOption);
    parser.addOption(cacheDirOption);
    parser.addOption(configDirOption);
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
