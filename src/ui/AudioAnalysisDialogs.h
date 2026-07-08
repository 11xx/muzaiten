#pragma once

#include "ui/AudioAnalysisData.h"

#include <QDialog>

#include <functional>

class Database;
class QFormLayout;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

class AudioAnalysisStatusDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AudioAnalysisStatusDialog(
        const QString &featuresPath,
        std::function<AudioAnalysisData::LiveStatus()> liveStatus = {},
        QWidget *parent = nullptr);

private:
    void refreshLiveStatus();

    std::function<AudioAnalysisData::LiveStatus()> m_liveStatus;
    QLabel *m_liveRunning = nullptr;
    QLabel *m_liveProgress = nullptr;
    QLabel *m_liveRate = nullptr;
    QLabel *m_liveEta = nullptr;
    QLabel *m_liveElapsed = nullptr;
    QLabel *m_livePower = nullptr;
    QTimer *m_liveTimer = nullptr;
};

class DuplicateCopiesDialog final : public QDialog {
    Q_OBJECT

public:
    DuplicateCopiesDialog(Database *db, const QString &featuresPath, QWidget *parent = nullptr);

private:
    void refresh();
    void showGroup(int row);
    void pinSelectedCopy();
    void unpinSelectedGroup();
    void updateButtons();
    qint64 selectedGroupId() const;
    QString selectedCopyPath() const;

    Database *m_db = nullptr;
    QString m_featuresPath;
    QLabel *m_status = nullptr;
    QTableWidget *m_groupsTable = nullptr;
    QTableWidget *m_copiesTable = nullptr;
    QPushButton *m_pinButton = nullptr;
    QPushButton *m_unpinButton = nullptr;
    QVector<AudioAnalysisData::DuplicateGroup> m_groups;
};
