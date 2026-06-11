#pragma once

#include <QKeySequence>
#include <QMap>
#include <QString>
#include <QVector>
#include <utility>

using KeyBindingMap = QMap<QKeySequence, QString>;

// Human-readable key/action rows for views whose bindings are handled directly
// in event filters (no profile map). Each such view exposes a static
// keyBindingReference() next to its key handler so the Keybinds dialog stays in
// sync with the code that actually handles the keys.
using KeyBindingReferenceList = QVector<std::pair<QString, QString>>;

struct KeyBindingProfile {
    QString name;
    QString label;
    QString hintText;
    KeyBindingMap bindings;
};
