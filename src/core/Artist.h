#pragma once

#include <QString>

struct Artist {
    qint64 id = 0;
    QString name;
    QString sortName;
    int albumCount = 0;
};

