#include "ui/QueueScreen.h"

#include "ui/QueueTable.h"

#include <QVBoxLayout>

QueueScreen::QueueScreen(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);

    m_table = new QueueTable(QueueTablePreset::FullScreen, this);
    m_table->setTableBorders(panelAllBorders());
    layout->addWidget(m_table, 1);

    connect(m_table, &QueueTable::trackActivated, this, &QueueScreen::queueTrackActivated);
    connect(m_table, &QueueTable::trackRatingChanged, this, &QueueScreen::queueTrackRatingChanged);
    connect(m_table, &QueueTable::rowsMoveRequested, this, &QueueScreen::queueRowsMoveRequested);
    connect(m_table, &QueueTable::rowsRemoveRequested, this, &QueueScreen::queueRowsRemoveRequested);
    connect(m_table, &QueueTable::removeAllMissingTracksRequested, this, &QueueScreen::removeAllMissingTracksRequested);
    connect(m_table, &QueueTable::clearRequested, this, &QueueScreen::queueClearRequested);
    connect(m_table, &QueueTable::clearPlayNextPriorityRequested, this, &QueueScreen::clearPlayNextPriorityRequested);
    connect(m_table, &QueueTable::saveQueueAsRequested, this, &QueueScreen::saveQueueAsRequested);
    connect(m_table, &QueueTable::restorePreviousQueueRequested, this, &QueueScreen::restorePreviousQueueRequested);
    connect(m_table, &QueueTable::unlinkFromPlaylistRequested, this, &QueueScreen::unlinkQueueFromPlaylistRequested);
    connect(m_table, &QueueTable::findFileRequested, this, &QueueScreen::findFileRequested);
    connect(m_table, &QueueTable::addToPlaylistRequested, this, &QueueScreen::addToPlaylistRequested);
    connect(m_table, &QueueTable::propertiesRequested, this, &QueueScreen::propertiesRequested);
    connect(m_table, &QueueTable::startRadioRequested, this, &QueueScreen::startRadioRequested);
    connect(m_table, &QueueTable::trackLibraryRequested, this, &QueueScreen::trackLibraryRequested);
    connect(m_table, &QueueTable::viewSettingsChanged, this, &QueueScreen::viewSettingsChanged);
}

void QueueScreen::setQueueStore(QueueStore *store)
{
    m_table->setQueueStore(store);
}

QString QueueScreen::viewSettingsJson() const
{
    return m_table->viewSettingsJson();
}

void QueueScreen::applyViewSettingsJson(const QString &json)
{
    m_table->applyViewSettingsJson(json);
}

void QueueScreen::resetViewSettings()
{
    m_table->resetViewSettings();
}

void QueueScreen::setNavigationScrollPadding(int rows)
{
    m_table->setNavigationScrollPadding(rows);
}

void QueueScreen::setKeyBindingProfileName(const QString &name)
{
    m_table->setKeyBindingProfileName(name);
}

QString QueueScreen::keyBindingProfileName() const
{
    return m_table->keyBindingProfileName();
}

void QueueScreen::focusQueue()
{
    m_table->navigationWidget()->setFocus(Qt::ShortcutFocusReason);
}

void QueueScreen::revealCurrentPlaying()
{
    m_table->revealCurrentPlaying();
}

void QueueScreen::setQueueIsPlaylistSourced(bool sourced)
{
    m_table->setQueueIsPlaylistSourced(sourced);
}
