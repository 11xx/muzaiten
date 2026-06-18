#pragma once

#include <QString>

class AppCore;

namespace DemoScreens {

struct Options {
    QString outputDir;
    QString searchQuery;
    QString artistName;
    bool searchVideo = false;
    int searchKeyDelayMs = 120;
};

bool capture(AppCore &core, const Options &options, QString *error);

} // namespace DemoScreens
