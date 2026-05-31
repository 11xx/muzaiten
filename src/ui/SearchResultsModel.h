#pragma once

#include "search/SearchIndex.h"
#include "search/SearchRecord.h"

#include <QAbstractListModel>
#include <QVector>

class SearchResultsModel : public QAbstractListModel {
    Q_OBJECT
public:
    // Custom roles
    enum Roles {
        // Returns the SearchRecord for the result row.
        SearchRecordRole = Qt::UserRole,
    };

    explicit SearchResultsModel(QObject *parent = nullptr);

    // Update the displayed results. Each ScoredResult embeds the matching record.
    void setResults(QVector<Search::ScoredResult> results);

    void clear();

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
    QVector<Search::ScoredResult> m_results;
};
