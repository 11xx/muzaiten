#include "playlist/PlaylistMatchIndex.h"

#include "db/Database.h"
#include "search/Exclusion.h"
#include "search/RankConfig.h"
#include "search/SearchRecord.h"

#include <QString>

#include <algorithm>

namespace PlaylistImport {

Search::SearchIndex buildMatchIndex(Database &db)
{
    QVector<Search::SearchRecord> records = db.allTracksForSearch();
    const Search::RankConfig rank =
        Search::RankConfig::fromJsonString(db.setting(QStringLiteral("search.ranking")));
    QVector<Search::ExcludeMatcher> excludes;
    excludes.reserve(rank.excludes.size());
    for (const Search::ExcludeRule &rule : rank.excludes) {
        Search::ExcludeMatcher matcher(rule);
        if (matcher.isValid()) {
            excludes.append(matcher);
        }
    }
    if (!excludes.isEmpty()) {
        records.erase(std::remove_if(records.begin(), records.end(),
                          [&excludes](const Search::SearchRecord &rec) {
                              for (const Search::ExcludeMatcher &m : excludes) {
                                  if (m.matches(rec)) {
                                      return true;
                                  }
                              }
                              return false;
                          }),
                      records.end());
    }
    Search::SearchIndex index;
    index.build(records);
    return index;
}

} // namespace PlaylistImport
