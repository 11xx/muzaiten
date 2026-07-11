// Keybinds-dialog reference tables for views whose keys are handled directly
// in event filters (SearchView::handleNavKey, PlaylistView::eventFilter).
// They live in this small standalone TU so the KeybindingsDialog test can link
// them without pulling in the full views. Update these together with the
// handlers they mirror.

#include "ui/PlaylistView.h"
#include "ui/SearchView.h"

KeyBindingReferenceList SearchView::keyBindingReference()
{
    return {
        {QStringLiteral("Down / Ctrl+N"), QStringLiteral("Move down")},
        {QStringLiteral("Up / Ctrl+P"), QStringLiteral("Move up")},
        {QStringLiteral("PageDown / PageUp"), QStringLiteral("Page results")},
        {QStringLiteral("Home / End"), QStringLiteral("First / last result")},
        {QStringLiteral("Return"), QStringLiteral("Add selected results to queue")},
        {QStringLiteral("Alt+Return"), QStringLiteral("Play selected results now")},
        {QStringLiteral("Tab"), QStringLiteral("Toggle result mark, move down")},
        {QStringLiteral("Ctrl+Space"), QStringLiteral("Toggle result mark")},
        {QStringLiteral("Ctrl+A"), QStringLiteral("Select all results")},
        {QStringLiteral("a"), QStringLiteral("Add selection to playlist… (browse mode)")},
        {QStringLiteral("Ctrl+F"), QStringLiteral("Toggle fuzzy mode")},
        {QStringLiteral("F5"), QStringLiteral("Refresh search index")},
        {QStringLiteral("/ or Ctrl+S"), QStringLiteral("Return to search box (browse mode)")},
        {QStringLiteral("Esc / Ctrl+G"), QStringLiteral("Clear search, then leave input")},
        {QStringLiteral("Ctrl+Wheel"), QStringLiteral("Adjust result row height")},
    };
}

KeyBindingReferenceList PlaylistView::keyBindingReference()
{
    // One section per pane; rows with an empty key render as headings.
    return {
        {QString(), QStringLiteral("Playlists pane")},
        {QStringLiteral("j / n"), QStringLiteral("Move down")},
        {QStringLiteral("k / p"), QStringLiteral("Move up")},
        {QStringLiteral("Shift+J / Shift+K"), QStringLiteral("Extend selection down / up")},
        {QStringLiteral("l / Right"), QStringLiteral("Focus items pane")},
        {QStringLiteral("z"), QStringLiteral("Fold/unfold saved-queue group")},
        {QStringLiteral("Return"), QStringLiteral("Play playlist (replace queue)")},
        {QStringLiteral("Space"), QStringLiteral("Add playlist to queue")},
        {QStringLiteral("= / +"), QStringLiteral("Add song… (search modal)")},
        {QStringLiteral("a / Insert"), QStringLiteral("New playlist")},
        {QStringLiteral("r / F2"), QStringLiteral("Rename playlist")},
        {QStringLiteral("c"), QStringLiteral("Edit playlist comment")},
        {QStringLiteral("Ctrl+D / Delete"), QStringLiteral("Delete selection (playlists / saved queues)")},
        {QStringLiteral("u"), QStringLiteral("Undo last item change")},
        {QStringLiteral("x"), QStringLiteral("Export playlist (m3u8/csv)")},
        {QStringLiteral("i"), QStringLiteral("Import… (paste/m3u/csv, matched against library)")},
        {QStringLiteral("T"), QStringLiteral("Toggle created-date display")},
        {QString(), QStringLiteral("Items pane")},
        {QStringLiteral("j / n"), QStringLiteral("Move down")},
        {QStringLiteral("k / p"), QStringLiteral("Move up")},
        {QStringLiteral("h / Left"), QStringLiteral("Back to playlists pane")},
        {QStringLiteral("Shift+N / Shift+P"), QStringLiteral("Move item down / up (reorder)")},
        {QStringLiteral("Return"), QStringLiteral("Play item (replace queue)")},
        {QStringLiteral("Space"), QStringLiteral("Add selected items to queue")},
        {QStringLiteral("= / +"), QStringLiteral("Add song… (search modal)")},
        {QStringLiteral("a"), QStringLiteral("Add selection to another playlist…")},
        {QStringLiteral("e"), QStringLiteral("Edit item match (re-open search)")},
        {QStringLiteral("c"), QStringLiteral("Edit item comment")},
        {QStringLiteral("Ctrl+D / Delete"), QStringLiteral("Remove selected items")},
        {QStringLiteral("u"), QStringLiteral("Undo last reorder/removal")},
        {QStringLiteral("Drag"), QStringLiteral("Reorder items (mouse)")},
        {QStringLiteral("s"), QStringLiteral("Cycle sort (display-only)")},
        {QStringLiteral("i"), QStringLiteral("Track properties")},
        {QStringLiteral("o"), QStringLiteral("Jump to current song when this playlist is playing")},
        {QStringLiteral("Ctrl+Wheel"), QStringLiteral("Adjust row height")},
    };
}
