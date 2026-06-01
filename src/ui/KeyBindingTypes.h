#pragma once

#include <QKeySequence>
#include <QMap>
#include <QString>

using KeyBindingMap = QMap<QKeySequence, QString>;

struct KeyBindingProfile {
    QString name;
    QString label;
    QString hintText;
    KeyBindingMap bindings;
};
