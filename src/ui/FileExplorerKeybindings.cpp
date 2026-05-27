#include "ui/FileExplorerKeybindings.h"

QVector<KeyBindingProfile> defaultKeyBindingProfiles()
{
    using namespace ExplorerAction;
    QVector<KeyBindingProfile> profiles;

    {
        KeyBindingProfile p;
        p.name = QStringLiteral("vim");
        p.label = QStringLiteral("Vim-style (j/k/h/l)");
        p.hintText = QStringLiteral("j\u2193 k\u2191 h\u2190 l\u2192  ggG  Spc\u25b6 a+ p\u25b6\u25b6 if ~ Esc");
        p.bindings = {
            {Qt::Key_J, MoveDown},
            {Qt::Key_K, MoveUp},
            {Qt::Key_H, NavigateUp},
            {Qt::Key_L, NavigateIn},
            {Qt::Key_G, ScrollToBottom},
            {QKeySequence(Qt::Key_Space), PlaySelected},
            {Qt::Key_A, AddToQueue},
            {Qt::Key_P, PlayNext},
            {Qt::Key_I, ImportDirectory},
            {Qt::Key_F, FindFile},
            {Qt::Key_AsciiTilde, GoHome},
            {Qt::Key_Escape, Escape},
        };
        profiles.push_back(p);
    }

    {
        KeyBindingProfile p;
        p.name = QStringLiteral("dired");
        p.label = QStringLiteral("Emacs/Dired-style");
        p.hintText = QStringLiteral("n\u2193 p\u2191  Spc\u25b6  s+ !\u25b6\u25b6  if ~ q");
        p.bindings = {
            {Qt::Key_N, MoveDown},
            {Qt::Key_P, MoveUp},
            {QKeySequence(Qt::ControlModifier | Qt::Key_N), MoveDown},
            {QKeySequence(Qt::ControlModifier | Qt::Key_P), MoveUp},
            {QKeySequence(Qt::Key_Space), PlaySelected},
            {Qt::Key_S, AddToQueue},
            {Qt::Key_Exclam, PlayNext},
            {Qt::Key_I, ImportDirectory},
            {Qt::Key_F, FindFile},
            {Qt::Key_AsciiTilde, GoHome},
            {Qt::Key_Q, Escape},
            {Qt::Key_Escape, Escape},
            {Qt::Key_Home, ScrollToTop},
            {Qt::Key_End, ScrollToBottom},
            {QKeySequence(Qt::ControlModifier | Qt::Key_V), PageDown},
            {QKeySequence(Qt::AltModifier | Qt::Key_V), PageUp},
        };
        profiles.push_back(p);
    }

    {
        KeyBindingProfile p;
        p.name = QStringLiteral("dired_hjkl");
        p.label = QStringLiteral("Dired-style + hjkl arrows");
        p.hintText = QStringLiteral("j\u2193 k\u2191 h\u2190 l\u2192  n\u2193 p\u2191  Spc\u25b6  s+ !\u25b6\u25b6  if ~ q");
        p.bindings = {
            {Qt::Key_J, MoveDown},
            {Qt::Key_K, MoveUp},
            {Qt::Key_H, NavigateUp},
            {Qt::Key_L, NavigateIn},
            {Qt::Key_N, MoveDown},
            {Qt::Key_P, MoveUp},
            {QKeySequence(Qt::Key_Space), PlaySelected},
            {Qt::Key_S, AddToQueue},
            {Qt::Key_Exclam, PlayNext},
            {Qt::Key_I, ImportDirectory},
            {Qt::Key_F, FindFile},
            {Qt::Key_AsciiTilde, GoHome},
            {Qt::Key_Q, Escape},
            {Qt::Key_Escape, Escape},
        };
        profiles.push_back(p);
    }

    return profiles;
}

KeyBindingMap bindingMapForProfile(const QString &name)
{
    for (const KeyBindingProfile &profile : defaultKeyBindingProfiles()) {
        if (profile.name == name) {
            return profile.bindings;
        }
    }
    return {};
}

QStringList availableProfileNames()
{
    QStringList names;
    for (const KeyBindingProfile &profile : defaultKeyBindingProfiles()) {
        names.push_back(profile.name);
    }
    return names;
}
