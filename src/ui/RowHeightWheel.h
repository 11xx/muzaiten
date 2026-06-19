#pragma once

#include <QWheelEvent>

#include <algorithm>

namespace ui {

// Shared Ctrl+wheel row-height gesture used by the table/list views. When `wheel`
// carries the Control modifier, nudges `current` by ±`step` (default 2) per
// scroll direction, clamps the result to [minHeight, maxHeight], hands it to
// `apply`, accepts the event and returns true. Returns false when Control is
// absent so the caller falls through to base handling.
//
// Only the gesture mechanics are shared — the bounds stay per-view on purpose
// (the artist sidebar caps lower than the tables; the fullscreen queue and the
// playlist item table floor higher), so each call site passes its own limits.
template <typename ApplyFn>
bool applyCtrlWheelRowHeight(QWheelEvent *wheel, int current, int minHeight, int maxHeight,
                             ApplyFn &&apply, int step = 2)
{
    if (!(wheel->modifiers() & Qt::ControlModifier)) {
        return false;
    }
    const int delta = wheel->angleDelta().y() > 0 ? step : -step;
    apply(std::clamp(current + delta, minHeight, maxHeight));
    wheel->accept();
    return true;
}

} // namespace ui
