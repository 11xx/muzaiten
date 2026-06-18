#pragma once

#include <QEvent>
#include <QFont>
#include <QHeaderView>
#include <QPalette>
#include <QStyleOptionHeader>

class QuietHeaderView final : public QHeaderView {
public:
    explicit QuietHeaderView(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QHeaderView(orientation, parent)
    {
        applyQuietFont();
    }

protected:
    void initStyleOption(QStyleOptionHeader *option) const override
    {
        QHeaderView::initStyleOption(option);
        applyQuietPalette(option);
    }

    void initStyleOptionForIndex(QStyleOptionHeader *option, int logicalIndex) const override
    {
        QHeaderView::initStyleOptionForIndex(option, logicalIndex);
        applyQuietPalette(option);
    }

    void changeEvent(QEvent *event) override
    {
        QHeaderView::changeEvent(event);
        if (event != nullptr
            && (event->type() == QEvent::ApplicationFontChange
                || event->type() == QEvent::FontChange
                || event->type() == QEvent::StyleChange)) {
            applyQuietFont();
        }
    }

private:
    void applyQuietFont()
    {
        QFont quietFont = font();
        quietFont.setWeight(QFont::Normal);
        setFont(quietFont);
    }

    static void applyQuietPalette(QStyleOptionHeader *option)
    {
        if (option == nullptr) {
            return;
        }

        const QColor quietText = option->palette.color(QPalette::Disabled, QPalette::Text);
        for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            option->palette.setColor(group, QPalette::ButtonText, quietText);
            option->palette.setColor(group, QPalette::Text, quietText);
            option->palette.setColor(group, QPalette::WindowText, quietText);
        }
    }
};
