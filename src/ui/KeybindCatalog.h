#pragma once

#include "ui/KeyBindingTypes.h"

#include <QKeySequence>
#include <QList>
#include <QMap>
#include <QString>
#include <QVector>

class QKeyEvent;

// An action a screen can bind, with its user-facing label and group.
struct KeyActionInfo {
    QString id;
    QString label;
    QString group;
};

// A screen whose keys the user can change: its actions, in display order, and
// the presets the user starts from.
struct KeyBindingScope {
    QString id;
    QString label;
    QString settingsKey;
    QVector<KeyActionInfo> actions;
    QVector<KeyBindingProfile> presets;
    QString defaultPreset;
};

// The user's changes to one screen's preset: each listed action has exactly
// these keys, and an empty list leaves it unassigned. Actions not listed keep
// their preset keys.
using KeyBindingOverrides = QMap<QString, QList<QKeySequence>>;

namespace KeyBindingScopeId {
    inline constexpr auto Queue = "queue";
    inline constexpr auto Library = "library";
    inline constexpr auto Explorer = "explorer";
}

const QVector<KeyBindingScope> &editableKeyBindingScopes();
const KeyBindingScope *keyBindingScope(const QString &id);
QString keyActionLabel(const KeyBindingScope &scope, const QString &action);
KeyBindingMap presetBindings(const KeyBindingScope &scope, const QString &presetName);

// The preset with the overrides applied. An overridden action loses its preset
// keys; a key an override assigns leaves whichever action had it.
KeyBindingMap applyKeyBindingOverrides(const KeyBindingMap &preset, const KeyBindingOverrides &overrides);
QList<QKeySequence> keysForAction(const KeyBindingMap &bindings, const QString &action);

QString keyBindingOverridesToJson(const KeyBindingOverrides &overrides);
// Unknown actions, malformed keys and keys reserved by the window are dropped.
KeyBindingOverrides keyBindingOverridesFromJson(const QString &json, const KeyBindingScope &scope);

// One key as screens match it: symbol keys reached with Shift (!, ~, ?) match
// without the Shift modifier, so a binding and a key press agree.
QKeySequence normalizedKey(const QKeySequence &key);
QKeySequence keySequenceForEvent(const QKeyEvent *event);
QString keyBindingAction(const KeyBindingMap &bindings, const QKeyEvent *event);

// Keys the main window handles before any screen (view switching, repeat,
// shuffle). Returns the window's use of the key, or an empty string.
QString reservedWindowKeyUse(const QKeySequence &key);
KeyBindingReferenceList windowKeyBindingReference();

QString keyText(const QKeySequence &key);
