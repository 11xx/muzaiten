#pragma once

#include <QSqlDatabase>
#include <QString>

// A minimal key/value SQLite store for persistent UI/view + session state, kept
// separate from the library database so it lives under XDG_STATE_HOME and can be
// reset (or let go) independently of the music library data. Carries its own
// schema version in a `meta` table; deleting the file safely resets to defaults.
class SettingsStore final {
public:
    explicit SettingsStore(const QString &path);
    ~SettingsStore();

    SettingsStore(const SettingsStore &) = delete;
    SettingsStore &operator=(const SettingsStore &) = delete;

    bool isOpen() const;
    QString setting(const QString &key, const QString &fallback = {}) const;
    bool setSetting(const QString &key, const QString &value);
    bool removeSetting(const QString &key);

private:
    QString m_connectionName;
    QSqlDatabase m_db;
};
