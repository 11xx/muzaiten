#pragma once

#include "ui/KeyBindingTypes.h"

#include <QString>
#include <QVector>

namespace QueueAction {
    inline constexpr auto MoveDown       = "move_down";
    inline constexpr auto MoveUp         = "move_up";
    inline constexpr auto PageDown       = "page_down";
    inline constexpr auto PageUp         = "page_up";
    inline constexpr auto PlaySelected   = "play_selected";
    inline constexpr auto RemoveSelected = "remove_selected";
    inline constexpr auto ClearQueue     = "clear_queue";
    inline constexpr auto ClearPlayNext  = "clear_play_next";
    inline constexpr auto FindLibrary    = "find_library";
    inline constexpr auto FindFile       = "find_file";
    inline constexpr auto JumpPlaying    = "jump_playing";
    inline constexpr auto Search         = "search";
    inline constexpr auto SearchNext     = "search_next";
    inline constexpr auto SearchPrevious = "search_previous";
}

QVector<KeyBindingProfile> defaultQueueKeyBindingProfiles();
KeyBindingMap queueBindingMapForProfile(const QString &name);
QString defaultQueueKeyBindingProfileName();
