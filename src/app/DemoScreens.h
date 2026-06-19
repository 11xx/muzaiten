#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

class AppCore;

namespace DemoScreens {

struct Options {
    QString outputDir;
    QString searchQuery;
    QString artistName;
    QString albumTitle;
    QString nowPlayingQuery;
    QStringList colorSchemes;
    QSize windowSize = QSize(1440, 900);
    bool nowPlaying = false;
    bool searchVideo = false;
    int searchKeyDelayMs = 120;
    double nowPlayingPositionRatio = 2.0 / 3.0;
};

bool capture(AppCore &core, const Options &options, QString *error);

} // namespace DemoScreens
