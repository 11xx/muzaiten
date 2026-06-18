#pragma once

#include <QString>

class AppCore;

namespace DemoScreens {

bool capture(AppCore &core, const QString &outputDir, const QString &searchQuery, QString *error);

} // namespace DemoScreens
