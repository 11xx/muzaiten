#pragma once

#include "search/RankConfig.h"

#include <QDialog>

class QTableWidget;

// Settings panel for search "ranking": an ordered list of criteria that re-sort
// results, plus glob exclusion rules. Mirrors LinkRootsDialog.
class RankingDialog final : public QDialog {
    Q_OBJECT
public:
    explicit RankingDialog(QWidget *parent = nullptr);

    void setConfig(const Search::RankConfig &config);
    Search::RankConfig config() const;

private:
    void addCriterionRow(const Search::RankRule &rule);
    void addBlankCriterionRow();
    void removeSelectedCriteria();
    void moveCriterion(int delta);

    void addExcludeRow(const Search::ExcludeRule &rule);
    void addBlankExcludeRow();
    void removeSelectedExcludes();

    QTableWidget *m_criteria = nullptr;
    QTableWidget *m_excludes = nullptr;
};
