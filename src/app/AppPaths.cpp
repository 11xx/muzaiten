#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QDir>

namespace AppPaths {

namespace {

constexpr auto kAppSubdir = "muzaiten";

QString ensure(const QString &dir)
{
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
    }
    return dir;
}

// --state-root flag / MUZAITEN_STATE_ROOT env / --dev-state shortcut -> a single
// root under which every category lives as <root>/<category>. Empty if unset.
QString combinedRoot()
{
    if (qApp != nullptr) {
        const QString cliRoot = qApp->property("muzaiten.stateRoot").toString().trimmed();
        if (!cliRoot.isEmpty()) {
            return QDir(cliRoot).absolutePath();
        }
    }
    const QString envRoot = qEnvironmentVariable("MUZAITEN_STATE_ROOT").trimmed();
    if (!envRoot.isEmpty()) {
        return QDir(envRoot).absolutePath();
    }
    const bool dev = (qApp != nullptr && qApp->property("muzaiten.devState").toBool())
        || qEnvironmentVariableIsSet("MUZAITEN_DEV_STATE");
    if (dev) {
        // CWD-relative on purpose: the dev store is the same ./dev-state regardless
        // of which build directory the binary lives in.
        return QDir(QStringLiteral("dev-state")).absolutePath();
    }
    return {};
}

QString resolve(const char *flagProperty,
                const char *envVar,
                const char *rootSubdir,
                const char *xdgVar,
                const char *xdgFallback)
{
    if (qApp != nullptr) {
        const QString flag = qApp->property(flagProperty).toString().trimmed();
        if (!flag.isEmpty()) {
            return ensure(QDir(flag).absolutePath());
        }
    }
    const QString fromEnv = qEnvironmentVariable(envVar).trimmed();
    if (!fromEnv.isEmpty()) {
        return ensure(QDir(fromEnv).absolutePath());
    }
    const QString root = combinedRoot();
    if (!root.isEmpty()) {
        return ensure(QDir(root).filePath(QString::fromLatin1(rootSubdir)));
    }
    const QString xdg = qEnvironmentVariable(xdgVar).trimmed();
    const QString base = !xdg.isEmpty() ? xdg : QDir::homePath() + QString::fromLatin1(xdgFallback);
    return ensure(QDir(base).filePath(QString::fromLatin1(kAppSubdir)));
}

} // namespace

QString dataDir()
{
    return resolve("muzaiten.dataDir", "MUZAITEN_DATA_DIR", "data", "XDG_DATA_HOME", "/.local/share");
}

QString stateDir()
{
    return resolve("muzaiten.stateDir", "MUZAITEN_STATE_DIR", "state", "XDG_STATE_HOME", "/.local/state");
}

QString cacheDir()
{
    return resolve("muzaiten.cacheDir", "MUZAITEN_CACHE_DIR", "cache", "XDG_CACHE_HOME", "/.cache");
}

QString configDir()
{
    return resolve("muzaiten.configDir", "MUZAITEN_CONFIG_DIR", "config", "XDG_CONFIG_HOME", "/.config");
}

} // namespace AppPaths
