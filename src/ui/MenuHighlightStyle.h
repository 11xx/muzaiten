#pragma once

#include <QProxyStyle>

// Several themes mark the menu entry under the cursor with a flat grey card of
// their own, painted from their colours rather than from QPalette::Highlight,
// so neither a palette override nor a stylesheet rule reaches it. Taking over
// the fill is what is left, and this is the one place that does it.
//
// The entry keeps everything else the theme gives it: its metrics, its
// checkmark, its submenu arrow, its disabled look. Clearing the selected state
// before delegating is what stops the theme painting its card over the fill,
// and the text colour is then set to whatever that fill needs.
class MenuHighlightStyle : public QProxyStyle {
public:
    enum class Emphasis {
        // The palette's highlight at full strength, with highlighted text over
        // it. What a menu whose entries are commands wants.
        Solid,
        // The highlight softened against the window colour, with ordinary text
        // over it, for a menu whose entries are toggled repeatedly and would be
        // shouting at full strength.
        Soft,
    };

    // Default-constructed base: it proxies the application style rather than
    // owning one.
    explicit MenuHighlightStyle(Emphasis emphasis);

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget = nullptr) const override;

private:
    Emphasis m_emphasis;
};
