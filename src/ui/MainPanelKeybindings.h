#pragma once

#include "ui/KeyBindingTypes.h"

#include <QJsonArray>
#include <QString>
#include <QVector>

enum class MainPanelId {
    Queue,
    Artists,
    Albums,
    Tracks,
};

namespace MainPanelAction {
    inline constexpr auto MoveDown = "move_down";
    inline constexpr auto MoveUp = "move_up";
    inline constexpr auto PageDown = "page_down";
    inline constexpr auto PageUp = "page_up";
    inline constexpr auto FocusPrevious = "focus_previous";
    inline constexpr auto FocusNext = "focus_next";
    inline constexpr auto FocusQueue = "focus_queue";
    inline constexpr auto FocusTracks = "focus_tracks";
    inline constexpr auto Activate = "activate";
    inline constexpr auto PlayNow = "play_now";
    inline constexpr auto AddToQueue = "add_to_queue";
    inline constexpr auto AddToPlaylist = "add_to_playlist";
    inline constexpr auto PlayNext = "play_next";
    inline constexpr auto Mark = "mark";
    inline constexpr auto MarkAll = "mark_all";
    inline constexpr auto Unmark = "unmark";
    inline constexpr auto UnmarkAll = "unmark_all";
    inline constexpr auto Search = "search";
    inline constexpr auto SearchNext = "search_next";
    inline constexpr auto SearchPrevious = "search_previous";
    inline constexpr auto Escape = "escape";
}

QVector<KeyBindingProfile> defaultMainPanelKeyBindingProfiles();
KeyBindingMap mainPanelBindingMapForProfile(const QString &name);
QString defaultMainPanelKeyBindingProfileName();

QVector<MainPanelId> defaultMainPanelFocusOrder();
QString mainPanelIdToString(MainPanelId id);
bool mainPanelIdFromString(const QString &value, MainPanelId *id);
QJsonArray mainPanelFocusOrderToJson(const QVector<MainPanelId> &order);
QVector<MainPanelId> mainPanelFocusOrderFromJson(const QJsonArray &array);
