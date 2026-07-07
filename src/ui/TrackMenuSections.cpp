#include "ui/TrackMenuSections.h"

#include <QAction>
#include <QMenu>
#include <QString>

#include <algorithm>

namespace {

void addSeparatorIfNeeded(QMenu &menu)
{
    if (!menu.actions().isEmpty() && !menu.actions().last()->isSeparator()) {
        menu.addSeparator();
    }
}

QString suffixForCount(int trackCount)
{
    return trackCount > 1 ? QStringLiteral(" (%1)").arg(trackCount) : QString();
}

void addAction(QMenu &menu, const QString &label, const std::function<void()> &callback)
{
    if (!callback) {
        return;
    }
    QAction *action = menu.addAction(label);
    QObject::connect(action, &QAction::triggered, &menu, [callback]() {
        callback();
    });
}

void addCheckableAction(QMenu &menu,
                        const QString &label,
                        bool checked,
                        const std::function<void(bool)> &callback)
{
    if (!callback) {
        return;
    }
    QAction *action = menu.addAction(label);
    action->setCheckable(true);
    action->setChecked(checked);
    QObject::connect(action, &QAction::toggled, &menu, [callback](bool on) {
        callback(on);
    });
}

}

namespace TrackMenuSections {

void appendTrackSections(QMenu &menu, const Callbacks &callbacks, const State &state)
{
    const int trackCount = std::max(1, state.trackCount);
    const QString suffix = suffixForCount(trackCount);

    const bool hasPlayGroup = callbacks.playNow || callbacks.playNext || callbacks.addToQueue
        || callbacks.playNextTemporary || callbacks.addToQueueTemporary;
    if (hasPlayGroup) {
        addSeparatorIfNeeded(menu);
        addAction(menu, trackCount == 1 ? QStringLiteral("Play") : QStringLiteral("Play now%1").arg(suffix),
                  callbacks.playNow);
        addAction(menu, QStringLiteral("Play next%1").arg(suffix), callbacks.playNext);
        addAction(menu, QStringLiteral("Add to queue%1").arg(suffix), callbacks.addToQueue);
        addAction(menu, QStringLiteral("Play next (don't save to playlist)%1").arg(suffix),
                  callbacks.playNextTemporary);
        addAction(menu, QStringLiteral("Add to queue (don't save to playlist)%1").arg(suffix),
                  callbacks.addToQueueTemporary);
    }

    if (callbacks.addToPlaylist) {
        addSeparatorIfNeeded(menu);
        addAction(menu, QStringLiteral("Add to playlist…%1").arg(suffix), callbacks.addToPlaylist);
    }

    const bool hasRadioGroup = callbacks.startRadio || callbacks.setNeverRadio || callbacks.setNoLearn;
    if (hasRadioGroup) {
        addSeparatorIfNeeded(menu);
        addAction(menu, QStringLiteral("Start Radio"), callbacks.startRadio);
        addCheckableAction(menu, QStringLiteral("Never play on radio"), state.neverRadioChecked,
                           callbacks.setNeverRadio);
        addCheckableAction(menu, QStringLiteral("Don't learn from this"), state.noLearnChecked,
                           callbacks.setNoLearn);
    }

    const bool hasInspectGroup = callbacks.findInLibrary || callbacks.openContainingDirectory
        || callbacks.copyPath || callbacks.properties;
    if (hasInspectGroup) {
        addSeparatorIfNeeded(menu);
        addAction(menu, QStringLiteral("Find in library"), callbacks.findInLibrary);
        addAction(menu, QStringLiteral("Open containing directory"), callbacks.openContainingDirectory);
        addAction(menu, QStringLiteral("Copy path"), callbacks.copyPath);
        addAction(menu, QStringLiteral("Properties"), callbacks.properties);
    }
}

}
