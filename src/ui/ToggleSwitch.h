#pragma once

#include <QAbstractButton>

class QVariantAnimation;

// A pill switch for a boolean that takes effect the moment it is flipped, as
// opposed to a checkbox, which reads as a form value confirmed later.
//
// It is drawn rather than styled, so it keeps two obligations: every colour
// comes from the palette, and the track carries an outline whose weight fades
// as the switch turns on. Without that outline an off switch disappears on a
// theme where the track and the window are near enough the same colour.
class ToggleSwitch final : public QAbstractButton {
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVariantAnimation *m_slide = nullptr;
    qreal m_position = 0.0;
};
