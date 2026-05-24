#include "ui/AlbumArtFallback.h"

#include <QPalette>
#include <QString>

namespace AlbumArtFallback {

QString resourcePath(const QPalette &palette)
{
    const QColor window = palette.color(QPalette::Window);
    const QColor text = palette.color(QPalette::WindowText);
    return window.lightness() < text.lightness()
        ? QStringLiteral(":/artwork/album-fallback-dark.svg")
        : QStringLiteral(":/artwork/album-fallback-light.svg");
}

} // namespace AlbumArtFallback
