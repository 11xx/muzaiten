#pragma once

// QSplitter persistence helpers used by view classes that own their own splitter
// (PlaylistView, RightSidebar, MainWindow). Centralising the stability policy in
// one place fixes two classes of bugs seen across panes:
//
//   1. Garbage / transient sizes leaking into settings. When the window is too
//      small (or not yet laid out) QSplitter reports degenerate distributions
//      (one pane near zero, the other swallowing everything, or a total that is a
//      fraction of the real content size). Persisting those would slowly corrupt
//      the saved layout, so each save and each restore is gated by the same
//      "is this distribution sane?" check.
//
//   2. Programmatic setSizes() clobbering user-tuned layouts. Any code path
//      that calls setSizes() (initial defaults, resetViewSettings(), resize
//      redistribution, ...) should NOT be persisted. Only an actual drag by the
//      user — splitterMoved() — should update the saved sizes.
//
// Usage: the owning view keeps a `QList<int> m_userSizes` (the sizes to persist).
// On splitterMoved(), it refreshes m_userSizes from splitter->sizes() iff
// splitterSizesAreStable(). On save, it serialises m_userSizes (not the live
// sizes). On restore, it calls restoreSplitterIfStable() against the JSON; if
// the stored sizes are unstable (legacy/missing) the caller's defaults win.

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QSplitter>

namespace SplitterPersistence {

// Serialize splitter sizes to JSON. Empty input yields an empty array.
inline QJsonArray splitterSizesToJson(const QList<int> &sizes)
{
    QJsonArray array;
    for (int size : sizes) {
        array.append(size);
    }
    return array;
}

// Parse a JSON array back to a QList<int>. Used by both save (defensive, in case
// settings were hand-edited) and restore paths.
inline QList<int> splitterSizesFromJson(const QJsonArray &array)
{
    QList<int> sizes;
    sizes.reserve(array.size());
    for (const QJsonValue &value : array) {
        sizes.push_back(value.toInt());
    }
    return sizes;
}

// Stability policy: a saved/live distribution is considered "stable" (worth
// persisting or restoring) when every pane is at least its declared minimum and
// the total covers the declared minimum total. This rejects the degenerate
// distributions Qt produces before layout, during tiny/zero-size windows, or
// when one pane has been programmatically shrunk to ~0.
//
// `minimums` must list one minimum per splitter widget (order matches the
// splitter's child order); `minimumTotal` is a floor for the sum that is
// independent of the per-pane minimums (typically the smallest usable total
// across all panes).
inline bool splitterSizesAreStable(const QList<int> &sizes,
                                  const QList<int> &minimums,
                                  int minimumTotal)
{
    if (sizes.size() != minimums.size() || sizes.isEmpty()) {
        return false;
    }

    int total = 0;
    for (int i = 0; i < sizes.size(); ++i) {
        const int size = sizes.at(i);
        if (size < minimums.at(i)) {
            return false;
        }
        total += size;
    }
    return total >= minimumTotal;
}

// Restore splitter sizes from JSON iff they pass the stability check; otherwise
// leave the splitter at whatever sizes the caller already set (the caller's
// defaults). Returns true when the splitter was actually updated.
inline bool restoreSplitterIfStable(QSplitter *splitter,
                                    const QJsonArray &array,
                                    const QList<int> &minimums,
                                    int minimumTotal)
{
    if (splitter == nullptr) {
        return false;
    }
    const QList<int> sizes = splitterSizesFromJson(array);
    if (splitterSizesAreStable(sizes, minimums, minimumTotal)) {
        splitter->setSizes(sizes);
        return true;
    }
    return false;
}

} // namespace SplitterPersistence
