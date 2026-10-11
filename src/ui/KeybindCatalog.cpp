#include "ui/KeybindCatalog.h"

#include "ui/FileExplorerKeybindings.h"
#include "ui/MainPanelKeybindings.h"
#include "ui/QueueKeybindings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>

namespace {

QVector<KeyActionInfo> queueActions()
{
    using namespace QueueAction;
    const QString navigation = QStringLiteral("Navigation");
    const QString playback = QStringLiteral("Playback");
    const QString editing = QStringLiteral("Editing the queue");
    const QString search = QStringLiteral("Search");
    const QString elsewhere = QStringLiteral("Find elsewhere");
    return {
        {QString::fromLatin1(MoveDown), QStringLiteral("Move down"), navigation},
        {QString::fromLatin1(MoveUp), QStringLiteral("Move up"), navigation},
        {QString::fromLatin1(PageDown), QStringLiteral("Page down"), navigation},
        {QString::fromLatin1(PageUp), QStringLiteral("Page up"), navigation},
        {QString::fromLatin1(JumpPlaying), QStringLiteral("Jump to the playing track"), navigation},
        {QString::fromLatin1(PlaySelected), QStringLiteral("Play selected"), playback},
        {QString::fromLatin1(MoveItemDown), QStringLiteral("Move item down"), editing},
        {QString::fromLatin1(MoveItemUp), QStringLiteral("Move item up"), editing},
        {QString::fromLatin1(RemoveSelected), QStringLiteral("Remove selected"), editing},
        {QString::fromLatin1(ClearQueue), QStringLiteral("Clear the queue"), editing},
        {QString::fromLatin1(ClearPlayNext), QStringLiteral("Clear play-next priority"), editing},
        {QString::fromLatin1(AddToPlaylist), QStringLiteral("Add to playlist"), editing},
        {QString::fromLatin1(Search), QStringLiteral("Search the queue"), search},
        {QString::fromLatin1(SearchNext), QStringLiteral("Next match"), search},
        {QString::fromLatin1(SearchPrevious), QStringLiteral("Previous match"), search},
        {QString::fromLatin1(Escape), QStringLiteral("Close search"), search},
        {QString::fromLatin1(FindLibrary), QStringLiteral("Find in library"), elsewhere},
        {QString::fromLatin1(FindFile), QStringLiteral("Show in file explorer"), elsewhere},
    };
}

QVector<KeyActionInfo> libraryActions()
{
    using namespace MainPanelAction;
    const QString navigation = QStringLiteral("Navigation");
    const QString panels = QStringLiteral("Panels");
    const QString playback = QStringLiteral("Playback and queue");
    const QString marking = QStringLiteral("Marking");
    const QString search = QStringLiteral("Search");
    return {
        {QString::fromLatin1(MoveDown), QStringLiteral("Move down"), navigation},
        {QString::fromLatin1(MoveUp), QStringLiteral("Move up"), navigation},
        {QString::fromLatin1(PageDown), QStringLiteral("Page down"), navigation},
        {QString::fromLatin1(PageUp), QStringLiteral("Page up"), navigation},
        {QString::fromLatin1(FocusPrevious), QStringLiteral("Previous panel"), panels},
        {QString::fromLatin1(FocusNext), QStringLiteral("Next panel"), panels},
        {QString::fromLatin1(FocusQueue), QStringLiteral("Go to the queue"), panels},
        {QString::fromLatin1(FocusTracks), QStringLiteral("Go to tracks"), panels},
        {QString::fromLatin1(Activate), QStringLiteral("Open or narrow to the selection"), panels},
        {QString::fromLatin1(PlayNow), QStringLiteral("Play now"), playback},
        {QString::fromLatin1(AddToQueue), QStringLiteral("Add to queue"), playback},
        {QString::fromLatin1(PlayNext), QStringLiteral("Play next"), playback},
        {QString::fromLatin1(AddToPlaylist), QStringLiteral("Add to playlist"), playback},
        {QString::fromLatin1(MoveItemDown), QStringLiteral("Move queue item down"), playback},
        {QString::fromLatin1(MoveItemUp), QStringLiteral("Move queue item up"), playback},
        {QString::fromLatin1(Mark), QStringLiteral("Mark"), marking},
        {QString::fromLatin1(MarkAll), QStringLiteral("Mark all"), marking},
        {QString::fromLatin1(Unmark), QStringLiteral("Unmark"), marking},
        {QString::fromLatin1(UnmarkAll), QStringLiteral("Unmark all"), marking},
        {QString::fromLatin1(Search), QStringLiteral("Search this panel"), search},
        {QString::fromLatin1(SearchNext), QStringLiteral("Next match"), search},
        {QString::fromLatin1(SearchPrevious), QStringLiteral("Previous match"), search},
        {QString::fromLatin1(Escape), QStringLiteral("Close search or clear narrowing"), search},
    };
}

QVector<KeyActionInfo> explorerActions()
{
    using namespace ExplorerAction;
    const QString navigation = QStringLiteral("Navigation");
    const QString folders = QStringLiteral("Folders");
    const QString playback = QStringLiteral("Playback and queue");
    const QString library = QStringLiteral("Library");
    return {
        {QString::fromLatin1(MoveDown), QStringLiteral("Move down"), navigation},
        {QString::fromLatin1(MoveUp), QStringLiteral("Move up"), navigation},
        {QString::fromLatin1(PageDown), QStringLiteral("Page down"), navigation},
        {QString::fromLatin1(PageUp), QStringLiteral("Page up"), navigation},
        {QString::fromLatin1(ScrollToTop), QStringLiteral("Go to the top"), navigation},
        {QString::fromLatin1(ScrollToBottom), QStringLiteral("Go to the bottom"), navigation},
        {QString::fromLatin1(NavigateIn), QStringLiteral("Open folder"), folders},
        {QString::fromLatin1(NavigateUp), QStringLiteral("Parent folder"), folders},
        {QString::fromLatin1(Escape), QStringLiteral("Leave folder"), folders},
        {QString::fromLatin1(GoHome), QStringLiteral("Home folder"), folders},
        {QString::fromLatin1(GoToStart), QStringLiteral("Starting folder"), folders},
        {QString::fromLatin1(PlaySelected), QStringLiteral("Play selected"), playback},
        {QString::fromLatin1(AddToQueue), QStringLiteral("Add to queue"), playback},
        {QString::fromLatin1(PlayNext), QStringLiteral("Play next"), playback},
        {QString::fromLatin1(AddToPlaylist), QStringLiteral("Add to playlist"), playback},
        {QString::fromLatin1(ImportDirectory), QStringLiteral("Import folder"), library},
        {QString::fromLatin1(FindFile), QStringLiteral("Find the file"), library},
    };
}

QVector<KeyBindingScope> buildScopes()
{
    return {
        {QString::fromLatin1(KeyBindingScopeId::Queue), QStringLiteral("Queue"),
         QStringLiteral("queueScreen.keyBindingOverrides"), queueActions(), defaultQueueKeyBindingProfiles(),
         defaultQueueKeyBindingProfileName()},
        {QString::fromLatin1(KeyBindingScopeId::Library), QStringLiteral("Library"),
         QStringLiteral("mainPanel.keyBindingOverrides"), libraryActions(), defaultMainPanelKeyBindingProfiles(),
         defaultMainPanelKeyBindingProfileName()},
        {QString::fromLatin1(KeyBindingScopeId::Explorer), QStringLiteral("File explorer"),
         QStringLiteral("fileExplorer.keyBindingOverrides"), explorerActions(), defaultKeyBindingProfiles(),
         QStringLiteral("vim")},
    };
}

// Punctuation a layout may report with Shift held. Letters, digits and Space
// keep Shift, since Shift+N and Shift+Space are distinct keys.
bool isShiftedSymbol(int key)
{
    return (key >= Qt::Key_Exclam && key <= Qt::Key_Slash) || (key >= Qt::Key_Colon && key <= Qt::Key_At)
        || (key >= Qt::Key_BracketLeft && key <= Qt::Key_QuoteLeft)
        || (key >= Qt::Key_BraceLeft && key <= Qt::Key_AsciiTilde);
}

} // namespace

const QVector<KeyBindingScope> &editableKeyBindingScopes()
{
    static const QVector<KeyBindingScope> scopes = buildScopes();
    return scopes;
}

const KeyBindingScope *keyBindingScope(const QString &id)
{
    for (const KeyBindingScope &scope : editableKeyBindingScopes()) {
        if (scope.id == id) {
            return &scope;
        }
    }
    return nullptr;
}

QString keyActionLabel(const KeyBindingScope &scope, const QString &action)
{
    for (const KeyActionInfo &info : scope.actions) {
        if (info.id == action) {
            return info.label;
        }
    }
    return action;
}

KeyBindingMap presetBindings(const KeyBindingScope &scope, const QString &presetName)
{
    for (const KeyBindingProfile &preset : scope.presets) {
        if (preset.name == presetName) {
            return preset.bindings;
        }
    }
    for (const KeyBindingProfile &preset : scope.presets) {
        if (preset.name == scope.defaultPreset) {
            return preset.bindings;
        }
    }
    return {};
}

KeyBindingMap applyKeyBindingOverrides(const KeyBindingMap &preset, const KeyBindingOverrides &overrides)
{
    KeyBindingMap result;
    for (auto it = preset.cbegin(); it != preset.cend(); ++it) {
        if (!overrides.contains(it.value())) {
            result.insert(normalizedKey(it.key()), it.value());
        }
    }
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        for (const QKeySequence &key : it.value()) {
            if (!key.isEmpty()) {
                result.insert(normalizedKey(key), it.key());
            }
        }
    }
    return result;
}

QList<QKeySequence> keysForAction(const KeyBindingMap &bindings, const QString &action)
{
    QList<QKeySequence> keys;
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        if (it.value() == action) {
            keys.push_back(it.key());
        }
    }
    return keys;
}

ResolvedKeyBindings resolveKeyBindings(const KeyBindingMap &preset, const KeyBindingOverrides &overrides)
{
    ResolvedKeyBindings resolved;
    resolved.bindings = applyKeyBindingOverrides(preset, overrides);
    KeyBindingMap normalizedPreset;
    for (auto it = preset.cbegin(); it != preset.cend(); ++it) {
        normalizedPreset.insert(normalizedKey(it.key()), it.value());
    }
    for (auto it = resolved.bindings.cbegin(); it != resolved.bindings.cend(); ++it) {
        if (normalizedPreset.value(it.key()) != it.value()) {
            resolved.userKeys.insert(it.key());
        }
    }
    for (auto it = normalizedPreset.cbegin(); it != normalizedPreset.cend(); ++it) {
        if (!resolved.bindings.contains(it.key())) {
            resolved.removedKeys.insert(it.key());
        }
    }
    return resolved;
}

bool isTextKey(const QKeySequence &key)
{
    if (key.count() != 1) {
        return false;
    }
    const QKeyCombination combination = key[0];
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers() & ~Qt::ShiftModifier;
    const int code = combination.key();
    return modifiers == Qt::NoModifier && ((code >= Qt::Key_Space && code <= Qt::Key_AsciiTilde) || code >= 0xa0)
        && code < Qt::Key_Escape;
}

QString keyBindingOverridesToJson(const KeyBindingOverrides &overrides)
{
    QJsonObject object;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        QJsonArray keys;
        for (const QKeySequence &key : it.value()) {
            keys.append(key.toString(QKeySequence::PortableText));
        }
        object.insert(it.key(), keys);
    }
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

KeyBindingOverrides keyBindingOverridesFromJson(const QString &json, const KeyBindingScope &scope)
{
    KeyBindingOverrides overrides;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject()) {
        return overrides;
    }
    const QJsonObject object = document.object();
    for (const KeyActionInfo &action : scope.actions) {
        const QJsonValue value = object.value(action.id);
        if (!value.isArray()) {
            continue;
        }
        QList<QKeySequence> keys;
        for (const QJsonValue &entry : value.toArray()) {
            // fromString accepts nearly any text, so a stored key must read back
            // exactly as written.
            const QString text = entry.toString();
            const QKeySequence parsed = QKeySequence::fromString(text, QKeySequence::PortableText);
            if (parsed.count() < 1 || parsed.count() > 2 || parsed[0].key() == Qt::Key_unknown
                || (parsed.count() == 2 && parsed[1].key() == Qt::Key_unknown)
                || parsed.toString(QKeySequence::PortableText).compare(text, Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QKeySequence key = normalizedKey(parsed);
            if (reservedWindowKeyUse(key).isEmpty() && !keys.contains(key)) {
                keys.push_back(key);
            }
        }
        overrides.insert(action.id, keys);
    }
    return overrides;
}

QKeySequence normalizedKey(const QKeySequence &key)
{
    if (key.count() != 1) {
        return key;
    }
    const QKeyCombination combination = key[0];
    Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    if ((modifiers & Qt::ShiftModifier) && isShiftedSymbol(combination.key())) {
        modifiers &= ~Qt::ShiftModifier;
    }
    return QKeySequence(QKeyCombination(modifiers, combination.key()));
}

QKeySequence keySequenceForEvent(const QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return normalizedKey(QKeySequence(QKeyCombination(modifiers, static_cast<Qt::Key>(event->key()))));
}

QString keyBindingAction(const KeyBindingMap &bindings, const QKeyEvent *event)
{
    return bindings.value(keySequenceForEvent(event));
}

QString reservedWindowKeyUse(const QKeySequence &key)
{
    for (const auto &[text, use] : windowKeyBindingReference()) {
        if (normalizedKey(key) == normalizedKey(QKeySequence(text))) {
            return use;
        }
    }
    return {};
}

KeyBindingReferenceList windowKeyBindingReference()
{
    return {
        {QStringLiteral("1"), QStringLiteral("Queue; again reveals the playing track")},
        {QStringLiteral("2"), QStringLiteral("Library panels; again switches to the music explorer")},
        {QStringLiteral("3"), QStringLiteral("Library or free-roam file explorer")},
        {QStringLiteral("4"), QStringLiteral("Search; again refreshes the index")},
        {QStringLiteral("5"), QStringLiteral("Playlists")},
        {QStringLiteral("O"), QStringLiteral("Find the playing track")},
        {QStringLiteral("R"), QStringLiteral("Cycle repeat")},
        {QStringLiteral("S"), QStringLiteral("Cycle shuffle")},
    };
}

QString keyText(const QKeySequence &key)
{
    return key.toString(QKeySequence::NativeText);
}
