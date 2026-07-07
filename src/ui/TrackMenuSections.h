#pragma once

#include <functional>

class QMenu;

namespace TrackMenuSections {

struct Callbacks {
    std::function<void()> playNow;
    std::function<void()> playNext;
    std::function<void()> addToQueue;
    std::function<void()> playNextTemporary;
    std::function<void()> addToQueueTemporary;
    std::function<void()> addToPlaylist;
    std::function<void()> startRadio;
    std::function<void(bool)> setNeverRadio;
    std::function<void(bool)> setNoLearn;
    std::function<void()> findInLibrary;
    std::function<void()> openContainingDirectory;
    std::function<void()> copyPath;
    std::function<void()> properties;
};

struct State {
    int trackCount = 1;
    bool neverRadioChecked = false;
    bool noLearnChecked = false;
};

void appendTrackSections(QMenu &menu, const Callbacks &callbacks, const State &state);

}
