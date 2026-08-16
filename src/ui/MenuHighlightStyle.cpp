#include "ui/MenuHighlightStyle.h"

#include "ui/SelectionColors.h"

#include <QPainter>
#include <QStyleOptionMenuItem>

MenuHighlightStyle::MenuHighlightStyle(Emphasis emphasis)
    : QProxyStyle()
    , m_emphasis(emphasis)
{
}

void MenuHighlightStyle::drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                                     const QWidget *widget) const
{
    // Pressed as well as pointed at: a theme marks the two differently, the
    // second usually lighter still, and an entry that answered only for the
    // first would go pale under the cursor the moment it was clicked.
    const bool marked = option->state.testFlag(State_Selected) || option->state.testFlag(State_Sunken);
    const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
    if (element != CE_MenuItem || item == nullptr || !marked || !option->state.testFlag(State_Enabled)) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }

    QStyleOptionMenuItem adjusted(*item);
    if (m_emphasis == Emphasis::Solid) {
        painter->fillRect(adjusted.rect, option->palette.highlight());
        // Full-strength highlight carries its own foreground with it.
        const QColor text = option->palette.highlightedText().color();
        for (const QPalette::ColorRole role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) {
            adjusted.palette.setColor(role, text);
        }
    } else {
        // The soft fill stays near the window colour, so the text that was
        // readable before it is still the readable one; highlighted text is
        // chosen against a full-strength highlight and can vanish here.
        painter->fillRect(adjusted.rect,
                          SelectionColors::dimmedHighlight(option->palette.color(QPalette::Window),
                                                           option->palette.color(QPalette::Highlight),
                                                           SelectionColors::kSoftHighlightAlpha));
    }
    // Both marks are cleared, or the theme paints whichever one it owns back
    // over the fill.
    adjusted.state &= ~(State_Selected | State_Sunken);
    QProxyStyle::drawControl(element, &adjusted, painter, widget);
}
