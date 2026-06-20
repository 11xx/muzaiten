#include "playlist/PlaylistImportWorker.h"

#include "db/Database.h"
#include "search/Exclusion.h"
#include "search/RankConfig.h"

#include <QUuid>

#include <algorithm>

PlaylistImportWorker::PlaylistImportWorker(QString dbPath, QObject *parent)
    : QObject(parent)
    , m_dbPath(std::move(dbPath))
{
}

PlaylistImportWorker::~PlaylistImportWorker()
{
    delete m_db;
}

bool PlaylistImportWorker::ensureIndex()
{
    if (m_indexBuilt) {
        return true;
    }
    if (m_db == nullptr) {
        const QString connName = QStringLiteral("playlist-import-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_db = new Database(connName);
        if (!m_db->open(m_dbPath)) {
            const QString err = m_db->lastError();
            delete m_db;
            m_db = nullptr;
            emit error(err);
            return false;
        }
    }
    // Honor the user's "Search ranking" exclude rules: drop excluded tracks before
    // building the index so they never surface as import-match candidates (e.g.
    // .Trash/.test/backup directories that otherwise pollute MultiMatch sets).
    QVector<Search::SearchRecord> records = m_db->allTracksForSearch();
    const Search::RankConfig rank =
        Search::RankConfig::fromJsonString(m_db->setting(QStringLiteral("search.ranking")));
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
    m_index.build(records);
    m_indexBuilt = true;
    return true;
}

void PlaylistImportWorker::matchEntries(QVector<PlaylistImport::ImportEntry> entries)
{
    if (!ensureIndex()) {
        return;
    }
    QVector<PlaylistImportMatch> results;
    results.reserve(entries.size());
    const int total = static_cast<int>(entries.size());
    int done = 0;
    for (const PlaylistImport::ImportEntry &entry : entries) {
        PlaylistImportMatch match;
        match.entry = entry;
        match.outcome = PlaylistMatcher::match(m_index, entry);
        results.append(match);
        emit progress(++done, total);
    }
    emit finished(results);
}
