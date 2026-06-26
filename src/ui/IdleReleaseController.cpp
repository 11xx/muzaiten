#include "ui/IdleReleaseController.h"

#include <QEvent>
#include <QTimer>
#include <QWidget>

IdleReleaseController::IdleReleaseController(QWidget *page,
                                             std::function<void()> release,
                                             std::function<void()> restore,
                                             int idleMs,
                                             QObject *parent)
    : QObject(parent ? parent : page)
    , m_page(page)
    , m_release(std::move(release))
    , m_restore(std::move(restore))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(idleMs);
    connect(m_timer, &QTimer::timeout, this, &IdleReleaseController::onTimeout);

    if (m_page != nullptr) {
        m_page->installEventFilter(this);
        // A page that starts out hidden (every non-current stacked page) should
        // begin counting down immediately rather than only after its first show.
        if (m_page->isHidden()) {
            m_timer->start();
        }
    }
}

bool IdleReleaseController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_page) {
        if (event->type() == QEvent::Hide) {
            m_timer->start(); // (re)arm the idle countdown
        } else if (event->type() == QEvent::Show) {
            m_timer->stop();
            if (m_released) {
                m_released = false;
                if (m_restore) {
                    m_restore();
                }
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void IdleReleaseController::onTimeout()
{
    // Guard against a show that slipped in before the queued timeout ran.
    if (m_page != nullptr && m_page->isVisible()) {
        return;
    }
    if (m_release) {
        m_release();
    }
    m_released = true;
}
