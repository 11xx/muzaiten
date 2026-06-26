#pragma once

#include <QObject>

#include <functional>

class QWidget;
class QTimer;

// Frees a screen's heavy in-memory data once it has stayed hidden for an idle
// window, while keeping it resident across quick back-and-forth navigation.
//
// It watches a page widget's Show/Hide events (the events QStackedWidget emits
// as the current page changes): hiding arms a single-shot timer, showing
// disarms it. When the timer fires while the page is still hidden, the release
// callback runs. An optional restore callback runs the next time the page is
// shown, but only if a release actually happened in the meantime, so returning
// to a still-resident screen costs nothing.
//
// Watching events instead of subclassing lets this attach to plain container
// widgets (e.g. a QSplitter page) as well as custom views.
class IdleReleaseController final : public QObject {
    Q_OBJECT

public:
    // Default idle window before a hidden screen frees its data.
    static constexpr int kDefaultIdleMs = 60000; // 1 minute
    static constexpr int kDisabledIdleMs = 0;

    IdleReleaseController(QWidget *page,
                          std::function<void()> release,
                          std::function<void()> restore = {},
                          int idleMs = kDefaultIdleMs,
                          QObject *parent = nullptr);

    // True between a release firing and the subsequent restore-on-show.
    bool isReleased() const { return m_released; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void onTimeout();

    QWidget *m_page = nullptr;
    std::function<void()> m_release;
    std::function<void()> m_restore;
    QTimer *m_timer = nullptr;
    bool m_released = false;
};
