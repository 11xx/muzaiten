#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace AppPaths {

namespace {

constexpr auto kAppSubdir = "muzaiten";
constexpr auto kConfigFileName = "muzaiten.conf";

QString ensure(const QString &dir)
{
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
    }
    return dir;
}

QString expandUser(const QString &path)
{
    if (path == QStringLiteral("~")) {
        return QDir::homePath();
    }
    if (path.startsWith(QStringLiteral("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    return path;
}

// Reads [paths]<key> from the config file. Empty when the file or key is absent.
// The config file location never depends on the config file, so this is safe to
// call from path resolution (no recursion).
QString configPathValue(const char *key)
{
    const QString file = QDir(configDir()).filePath(QString::fromLatin1(kConfigFileName));
    if (!QFileInfo::exists(file)) {
        return {};
    }
    QSettings settings(file, QSettings::IniFormat);
    const QString value = settings.value(QStringLiteral("paths/") + QString::fromLatin1(key)).toString().trimmed();
    return value.isEmpty() ? QString() : QDir(expandUser(value)).absolutePath();
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
    // Config file layer (below CLI/env/root, above the XDG default). The config
    // directory itself is bootstrap and cannot be set from the config file.
    if (qstrcmp(rootSubdir, "config") != 0) {
        const QString fromConfig = configPathValue(rootSubdir);
        if (!fromConfig.isEmpty()) {
            return ensure(fromConfig);
        }
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

QString configFilePath()
{
    return QDir(configDir()).filePath(QString::fromLatin1(kConfigFileName));
}

void writeDefaultConfigIfMissing()
{
    const QString path = configFilePath();
    if (QFileInfo::exists(path)) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    // A fully-commented template: it has no effect until a user uncomments a key.
    // CLI flags and environment variables always override these values.
    file.write(
        "# muzaiten configuration (INI). Values here are used only when the matching\n"
        "# CLI flag or environment variable is not set; flags and env vars win.\n"
        "\n"
        "[paths]\n"
        "# Library database (default: $XDG_DATA_HOME/muzaiten or ~/.local/share/muzaiten).\n"
        "#data = ~/Music/muzaiten\n"
        "# UI preferences + session state (default: $XDG_STATE_HOME/muzaiten or ~/.local/state/muzaiten).\n"
        "#state = ~/.local/state/muzaiten\n"
        "# Artwork cache (default: $XDG_CACHE_HOME/muzaiten or ~/.cache/muzaiten).\n"
        "#cache = ~/.cache/muzaiten\n");
    file.close();
}

} // namespace AppPaths
