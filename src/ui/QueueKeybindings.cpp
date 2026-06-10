#include "ui/QueueKeybindings.h"

QVector<KeyBindingProfile> defaultQueueKeyBindingProfiles()
{
    using namespace QueueAction;

    KeyBindingProfile profile;
    profile.name = QStringLiteral("dired_hjkl");
    profile.label = QStringLiteral("Queue: dired + hjkl");
    profile.hintText = QStringLiteral("j/n↓ k/p↑ C-v/M-v page Ret play d remove c clear x clear-priority f file l library o playing / search Esc/C-g clear");
    profile.bindings = {
        {Qt::Key_J, MoveDown},
        {Qt::Key_N, MoveDown},
        {Qt::Key_Down, MoveDown},
        {QKeySequence(Qt::ControlModifier | Qt::Key_N), MoveDown},
        {Qt::Key_K, MoveUp},
        {Qt::Key_P, MoveUp},
        {Qt::Key_Up, MoveUp},
        {QKeySequence(Qt::ControlModifier | Qt::Key_P), MoveUp},
        {Qt::Key_PageDown, PageDown},
        {QKeySequence(Qt::ControlModifier | Qt::Key_V), PageDown},
        {Qt::Key_PageUp, PageUp},
        {QKeySequence(Qt::AltModifier | Qt::Key_V), PageUp},
        {Qt::Key_Return, PlaySelected},
        {Qt::Key_Enter, PlaySelected},
        {Qt::Key_Space, PlaySelected},
        {Qt::Key_D, RemoveSelected},
        {Qt::Key_Delete, RemoveSelected},
        {Qt::Key_C, ClearQueue},
        {Qt::Key_X, ClearPlayNext},
        {Qt::Key_F, FindFile},
        {Qt::Key_L, FindLibrary},
        {Qt::Key_O, JumpPlaying},
        {Qt::Key_Slash, Search},
        {QKeySequence(Qt::ControlModifier | Qt::Key_S), Search},
        {QKeySequence(Qt::AltModifier | Qt::Key_N), SearchNext},
        {QKeySequence(Qt::AltModifier | Qt::Key_P), SearchPrevious},
        {Qt::Key_Escape, Escape},
        {QKeySequence(Qt::ControlModifier | Qt::Key_G), Escape},
    };
    return {profile};
}

QString defaultQueueKeyBindingProfileName()
{
    return QStringLiteral("dired_hjkl");
}

KeyBindingMap queueBindingMapForProfile(const QString &name)
{
    const QString requested = name.isEmpty() ? defaultQueueKeyBindingProfileName() : name;
    for (const KeyBindingProfile &profile : defaultQueueKeyBindingProfiles()) {
        if (profile.name == requested) {
            return profile.bindings;
        }
    }
    for (const KeyBindingProfile &profile : defaultQueueKeyBindingProfiles()) {
        if (profile.name == defaultQueueKeyBindingProfileName()) {
            return profile.bindings;
        }
    }
    return {};
}
