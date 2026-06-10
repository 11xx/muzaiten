#pragma once

#include "ui/KeyBindingTypes.h"

#include <QString>
#include <QVector>

namespace ExplorerAction {
    inline constexpr auto MoveDown        = "move_down";
    inline constexpr auto MoveUp          = "move_up";
    inline constexpr auto NavigateUp      = "navigate_up";
    inline constexpr auto NavigateIn      = "navigate_in";
    inline constexpr auto ScrollToTop     = "scroll_to_top";
    inline constexpr auto ScrollToBottom  = "scroll_to_bottom";
    inline constexpr auto PageDown        = "page_down";
    inline constexpr auto PageUp          = "page_up";
    inline constexpr auto PlaySelected    = "play_selected";
    inline constexpr auto AddToQueue      = "add_to_queue";
    inline constexpr auto PlayNext        = "play_next";
    inline constexpr auto ImportDirectory = "import_directory";
    inline constexpr auto FindFile        = "find_file";
    inline constexpr auto AddToPlaylist   = "add_to_playlist";
    inline constexpr auto GoHome          = "go_home";
    inline constexpr auto GoToStart       = "go_to_start";
    inline constexpr auto Escape          = "escape";
}

QVector<KeyBindingProfile> defaultKeyBindingProfiles();
KeyBindingMap bindingMapForProfile(const QString &name);
QStringList availableProfileNames();
