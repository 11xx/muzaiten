#pragma once

#include <QVector>
#include <QString>
#include <QStringList>

class Database;

namespace GenreCuration {

struct AliasValidation {
    QString aliasFolded;
    QString canonicalFolded;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

struct ReportRow {
    QString genre;
    int df = 0;
    double idf = 0.0;
    QString canonical;
    QString status;
    QStringList sampleArtists;
    QStringList flags;
};

AliasValidation validateAlias(const QString &alias, const QString &canonical);
QString canonicalGenreInput(Database &db, const QString &genre, QString *error = nullptr);
QVector<ReportRow> buildReportRows(Database &db, int *taggedTrackTotal = nullptr);

} // namespace GenreCuration
