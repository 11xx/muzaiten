#pragma once

#include <QWidget>

class QAbstractScrollArea;
class QScrollBar;

// Floating vertical scrollbar overlay: attaches to a QAbstractScrollArea, hides
// the native vertical scrollbar, and floats a zero-gutter indicator over the
// viewport's right edge.
//
// Idle:    a faint 2px hairline showing the approximate handle position.
// Hovered: a bright rounded handle when the cursor enters a ghostZone-wide strip
//          on the right edge.  Dragging the handle scrolls the view.
//
// The overlay is parented to area->viewport() and manages its own lifetime.
class OverlayScrollBar final : public QWidget {
    Q_OBJECT

public:
    // Attaches a vertical overlay to area and hides the native vertical scrollbar.
    explicit OverlayScrollBar(QAbstractScrollArea *area, QWidget *parent = nullptr);

    // Convenience factory – typical call site: OverlayScrollBar::install(this).
    static void install(QAbstractScrollArea *area);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reposition();
    QRect handleRect() const;
    void setHovered(bool hovered);
    int viewportHeight() const;
    bool isScrollable() const;

    QAbstractScrollArea *m_area = nullptr;
    QScrollBar *m_scrollBar = nullptr;

    // px from the right viewport edge that triggers the hover/expand state
    static constexpr int ghostZone = 16;
    // visible handle width when expanded
    static constexpr int handleWidth = 10;
    // minimum handle length in px
    static constexpr int handleMinLen = 28;

    bool m_hovered = false;
    bool m_dragging = false;
    int m_dragStartPos = 0;   // cursor Y in viewport coords when drag began
    int m_dragStartValue = 0; // scrollbar value when drag began
    int m_dragTrack = 0;      // (viewport height - handle height) at drag start
};
