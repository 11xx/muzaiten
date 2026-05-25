#pragma once

#include <QString>

struct ScanRoot {
    int id = 0;
    QString name;
    QString path;
    bool scanEnabled = true;
    bool libraryEnabled = true;
    QString createdAt;
    QString updatedAt;
    QString lastScannedAt;
    QString lastError;
};
