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
    const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
    if (element != CE_MenuItem || item == nullptr || !option->state.testFlag(State_Selected)
        || !option->state.testFlag(State_Enabled)) {
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
    adjusted.state &= ~State_Selected;
    QProxyStyle::drawControl(element, &adjusted, painter, widget);
}
