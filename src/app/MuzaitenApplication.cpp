#include "app/MuzaitenApplication.h"

#include "Version.h"
#include "core/Track.h"
#include "ui/MainWindow.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>

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
    QCoreApplication::setOrganizationName(QStringLiteral("muzaiten"));
    QCoreApplication::setOrganizationDomain(QStringLiteral(MUZAITEN_APP_ID));

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
    const QCommandLineOption stateRootOption(QStringLiteral("state-root"), QStringLiteral("Store database, cache, and config under <path>."), QStringLiteral("path"));
    const QCommandLineOption xdgStateOption(QStringLiteral("xdg-state"), QStringLiteral("Use normal XDG state paths even when running from build/."));
    parser.addOption(verboseOption);
    parser.addOption(stateRootOption);
    parser.addOption(xdgStateOption);
    parser.process(*this);

    const bool verbose = parser.isSet(verboseOption) || qEnvironmentVariableIsSet("MUZAITEN_VERBOSE");
    configureLogging(verbose);
    setProperty("muzaiten.verbose", verbose);

    const QString envRoot = QString::fromLocal8Bit(qgetenv("MUZAITEN_STATE_ROOT")).trimmed();
    const QString cliRoot = parser.value(stateRootOption).trimmed();
    const QString stateRoot = !cliRoot.isEmpty() ? cliRoot : envRoot;
    if (!stateRoot.isEmpty()) {
        setProperty("muzaiten.stateRoot", QDir(stateRoot).absolutePath());
    }
    setProperty("muzaiten.devState", !parser.isSet(xdgStateOption));
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
