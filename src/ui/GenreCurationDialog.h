#pragma once

#include "reco/GenreCuration.h"

#include <QDialog>

class Database;
class QLineEdit;
class QListWidget;
class QTableWidget;

class GenreCurationDialog final : public QDialog {
    Q_OBJECT

public:
    explicit GenreCurationDialog(Database *db, QWidget *parent = nullptr);

private:
    void refreshAliases();
    void refreshIgnored();
    void refreshVocabulary();
    void addAlias();
    void editAlias();
    void removeAlias();
    void addIgnoredGenre();
    void removeIgnoredGenre();
    void setAlias(const QString &alias, const QString &canonical);
    void showError(const QString &message);

    Database *m_db = nullptr;
    QTableWidget *m_aliases = nullptr;
    QListWidget *m_ignored = nullptr;
    QLineEdit *m_vocabularyFilter = nullptr;
    QTableWidget *m_vocabulary = nullptr;
    QVector<GenreCuration::ReportRow> m_vocabularyRows;
};
