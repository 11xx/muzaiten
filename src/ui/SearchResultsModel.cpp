#include "ui/SearchResultsModel.h"

#include "search/SearchIndex.h"
#include "search/SearchRecord.h"

SearchResultsModel::SearchResultsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void SearchResultsModel::setResults(QVector<Search::ScoredResult> results)
{
    beginResetModel();
    m_results = std::move(results);
    endResetModel();
}

void SearchResultsModel::clear()
{
    beginResetModel();
    m_results.clear();
    endResetModel();
}

int SearchResultsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_results.size());
}

QVariant SearchResultsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_results.size())) {
        return {};
    }

    const Search::ScoredResult &sr = m_results[index.row()];

    switch (role) {
    case Qt::DisplayRole:
        return sr.rec.title;

    case SearchRecordRole:
        return QVariant::fromValue(sr.rec);

    case ScoredResultRole:
        return QVariant::fromValue(sr);

    default:
        return {};
    }
}
