#include "ui/MainPanelKeybindings.h"

#include <QSet>

QVector<KeyBindingProfile> defaultMainPanelKeyBindingProfiles()
{
    using namespace MainPanelAction;

    KeyBindingProfile profile;
    profile.name = QStringLiteral("dired_hjkl");
    profile.label = QStringLiteral("Main panels: dired + hjkl");
    profile.hintText = QStringLiteral("j/n↓ k/p↑ h← l→ q queue / search M-n/M-p matches Ret play Spc+ M-Spc▶▶");
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
        {Qt::Key_H, FocusPrevious},
        {Qt::Key_L, FocusNext},
        {Qt::Key_Q, FocusQueue},
        {Qt::Key_Slash, Search},
        {QKeySequence(Qt::AltModifier | Qt::Key_N), SearchNext},
        {QKeySequence(Qt::AltModifier | Qt::Key_P), SearchPrevious},
        {Qt::Key_Return, PlayNow},
        {Qt::Key_Enter, PlayNow},
        {QKeySequence(Qt::AltModifier | Qt::Key_Return), PlayNow},
        {QKeySequence(Qt::AltModifier | Qt::Key_Enter), PlayNow},
        {Qt::Key_Space, AddToQueue},
        {QKeySequence(Qt::AltModifier | Qt::Key_Space), PlayNext},
        {Qt::Key_Escape, Escape},
        {QKeySequence(Qt::ControlModifier | Qt::Key_G), Escape},
    };
    return {profile};
}

QString defaultMainPanelKeyBindingProfileName()
{
    return QStringLiteral("dired_hjkl");
}

KeyBindingMap mainPanelBindingMapForProfile(const QString &name)
{
    const QString requested = name.isEmpty() ? defaultMainPanelKeyBindingProfileName() : name;
    for (const KeyBindingProfile &profile : defaultMainPanelKeyBindingProfiles()) {
        if (profile.name == requested) {
            return profile.bindings;
        }
    }
    for (const KeyBindingProfile &profile : defaultMainPanelKeyBindingProfiles()) {
        if (profile.name == defaultMainPanelKeyBindingProfileName()) {
            return profile.bindings;
        }
    }
    return {};
}

QVector<MainPanelId> defaultMainPanelFocusOrder()
{
    return {
        MainPanelId::Queue,
        MainPanelId::Artists,
        MainPanelId::Albums,
        MainPanelId::Tracks,
    };
}

QString mainPanelIdToString(MainPanelId id)
{
    switch (id) {
    case MainPanelId::Queue:   return QStringLiteral("queue");
    case MainPanelId::Artists: return QStringLiteral("artists");
    case MainPanelId::Albums:  return QStringLiteral("albums");
    case MainPanelId::Tracks:  return QStringLiteral("tracks");
    }
    return QStringLiteral("artists");
}

bool mainPanelIdFromString(const QString &value, MainPanelId *id)
{
    if (value == QStringLiteral("queue")) {
        if (id != nullptr) *id = MainPanelId::Queue;
        return true;
    }
    if (value == QStringLiteral("artists")) {
        if (id != nullptr) *id = MainPanelId::Artists;
        return true;
    }
    if (value == QStringLiteral("albums")) {
        if (id != nullptr) *id = MainPanelId::Albums;
        return true;
    }
    if (value == QStringLiteral("tracks")) {
        if (id != nullptr) *id = MainPanelId::Tracks;
        return true;
    }
    return false;
}

QJsonArray mainPanelFocusOrderToJson(const QVector<MainPanelId> &order)
{
    QJsonArray array;
    for (MainPanelId id : order) {
        array.append(mainPanelIdToString(id));
    }
    return array;
}

QVector<MainPanelId> mainPanelFocusOrderFromJson(const QJsonArray &array)
{
    const QVector<MainPanelId> fallback = defaultMainPanelFocusOrder();
    if (array.size() != fallback.size()) {
        return fallback;
    }

    QVector<MainPanelId> order;
    QSet<QString> seen;
    order.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QString text = value.toString();
        MainPanelId id = MainPanelId::Artists;
        if (!mainPanelIdFromString(text, &id) || seen.contains(text)) {
            return fallback;
        }
        seen.insert(text);
        order.push_back(id);
    }
    return order.size() == fallback.size() ? order : fallback;
}
