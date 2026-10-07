#include "indexer/FeaturesPack.h"

#include "app/AppPaths.h"
#include "indexer/DspVersion.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QTextStream>
#include <QUuid>
#include <QVariant>

#include <cmath>

namespace {

const QStringList scalarKeys{
    QStringLiteral("tempo_bpm"), QStringLiteral("loudness_lufs"), QStringLiteral("loudness_std_db"),
    QStringLiteral("spectral_centroid_mean_hz"), QStringLiteral("spectral_centroid_std_hz"),
    QStringLiteral("spectral_flatness_mean"), QStringLiteral("zero_crossing_rate"),
    QStringLiteral("onset_rate_hz"), QStringLiteral("energy")};

[[noreturn]] void fail(int code, const QString &message)
{
    throw FeaturesPack::Error(code, message);
}

void exactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!keys.contains(it.key())) {
            fail(2, QStringLiteral("Unknown pack key: %1.").arg(it.key()));
        }
    }
    for (const QString &key : keys) {
        if (!object.contains(key)) {
            fail(2, QStringLiteral("Missing pack key: %1.").arg(key));
        }
    }
}

// Qt collapses duplicate JSON keys. Check the validated source before using it.
void uniqueKeys(const QByteArray &bytes)
{
    QList<QSet<QString>> objects;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes.at(i) == '{') {
            objects.append(QSet<QString>{});
        } else if (bytes.at(i) == '}') {
            objects.removeLast();
        } else if (bytes.at(i) == '"') {
            const qsizetype start = i++;
            while (i < bytes.size() && bytes.at(i) != '"') {
                if (bytes.at(i) == '\\') {
                    ++i;
                }
                ++i;
            }
            qsizetype next = i + 1;
            while (next < bytes.size() && (bytes.at(next) == ' ' || bytes.at(next) == '\t'
                                         || bytes.at(next) == '\r')) {
                ++next;
            }
            if (next < bytes.size() && bytes.at(next) == ':' && !objects.isEmpty()) {
                const QString key = QJsonDocument::fromJson("[" + bytes.mid(start, i - start + 1) + "]")
                                        .array().first().toString();
                if (objects.last().contains(key)) {
                    fail(2, QStringLiteral("Duplicate pack key: %1.").arg(key));
                }
                objects.last().insert(key);
            }
        }
    }
}

QJsonObject parseObject(const QByteArray &line)
{
    if (line.size() > FeaturesPack::MaximumLineBytes) {
        fail(2, QStringLiteral("Pack line exceeds 16 KiB."));
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(line);
    Q_UNUSED(text);
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (decoder.hasError() || error.error != QJsonParseError::NoError || !document.isObject()) {
        fail(2, QStringLiteral("Each pack line must be a complete UTF-8 JSON object."));
    }
    uniqueKeys(line);
    return document.object();
}

bool validHash(const QString &hash)
{
    if (hash.size() != 64) {
        return false;
    }
    for (QChar ch : hash) {
        if (!(ch >= u'0' && ch <= u'9') && !(ch >= u'a' && ch <= u'f')) {
            return false;
        }
    }
    return true;
}

void validateToken(const QJsonValue &value)
{
    const QString token = value.toString();
    if (!value.isString() || token.isEmpty() || token.size() > 128) {
        fail(2, QStringLiteral("Extractor and version must be 1 to 128 ASCII token characters."));
    }
    for (QChar ch : token) {
        if (!(ch >= u'a' && ch <= u'z') && !(ch >= u'A' && ch <= u'Z')
            && !(ch >= u'0' && ch <= u'9') && ch != u'-' && ch != u'_' && ch != u'.') {
            fail(2, QStringLiteral("Invalid extractor or version token."));
        }
    }
}

bool validScalar(const QString &key, const QJsonValue &scalar)
{
    if (scalar.isNull()) {
        return true;
    }
    const double number = scalar.toDouble();
    const bool unitInterval = key == QLatin1String("spectral_flatness_mean")
        || key == QLatin1String("zero_crossing_rate") || key == QLatin1String("energy");
    return scalar.isDouble() && std::isfinite(number)
        && (key == QLatin1String("loudness_lufs") || number >= 0.0) && (!unitInterval || number <= 1.0);
}

void validateRecord(const QJsonObject &record)
{
    exactKeys(record, {QStringLiteral("decode_hash"), QStringLiteral("extractor"),
                       QStringLiteral("version"), QStringLiteral("scalars")});
    if (!record.value(QStringLiteral("decode_hash")).isString()
        || !validHash(record.value(QStringLiteral("decode_hash")).toString())) {
        fail(2, QStringLiteral("decode_hash must be a lowercase SHA-256 hex string."));
    }
    validateToken(record.value(QStringLiteral("extractor")));
    validateToken(record.value(QStringLiteral("version")));
    const QJsonValue value = record.value(QStringLiteral("scalars"));
    if (!value.isObject()) {
        fail(2, QStringLiteral("Pack scalars must be an object."));
    }
    const QJsonObject scalars = value.toObject();
    exactKeys(scalars, QSet<QString>(scalarKeys.begin(), scalarKeys.end()));
    for (const QString &key : scalarKeys) {
        if (!validScalar(key, scalars.value(key))) {
            fail(2, QStringLiteral("Invalid scalar value: %1.").arg(key));
        }
    }
}

QList<QJsonObject> readPack(const QString &path)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
        fail(4, QStringLiteral("Could not open feature pack."));
    }
    if (file.size() > FeaturesPack::MaximumBytes) {
        fail(2, QStringLiteral("Feature pack exceeds 64 MiB."));
    }
    const QByteArray bytes = file.read(FeaturesPack::MaximumBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        fail(4, QStringLiteral("Could not read feature pack."));
    }
    if (bytes.size() > FeaturesPack::MaximumBytes || bytes.isEmpty() || !bytes.endsWith('\n')) {
        fail(2, QStringLiteral("Pack must fit within 64 MiB and end with a newline."));
    }
    qsizetype offset = 0;
    const auto nextLine = [&]() {
        const qsizetype end = bytes.indexOf('\n', offset);
        const QByteArray line = bytes.mid(offset, end - offset);
        offset = end + 1;
        return parseObject(line);
    };
    const QJsonObject header = nextLine();
    exactKeys(header, {QStringLiteral("format"), QStringLiteral("version"), QStringLiteral("records")});
    if (header.value(QStringLiteral("format")) != QJsonValue(QStringLiteral("muzaiten-features-pack"))
        || !header.value(QStringLiteral("version")).isDouble()
        || header.value(QStringLiteral("version")).toDouble() != 1.0) {
        fail(2, QStringLiteral("Unsupported feature-pack format or version; expected muzaiten-features-pack v1."));
    }
    const QJsonValue count = header.value(QStringLiteral("records"));
    const double number = count.toDouble(-1);
    if (!count.isDouble() || number < 0 || number > FeaturesPack::MaximumRecords || std::floor(number) != number) {
        fail(2, QStringLiteral("Pack record count must be an integer from 0 to 100000."));
    }
    QList<QJsonObject> records;
    QSet<QString> hashes;
    while (offset < bytes.size()) {
        if (records.size() >= FeaturesPack::MaximumRecords || records.size() >= static_cast<int>(number)) {
            fail(2, QStringLiteral("Pack has more records than declared or allowed."));
        }
        const QJsonObject record = nextLine();
        validateRecord(record);
        const QString hash = record.value(QStringLiteral("decode_hash")).toString();
        if (hashes.contains(hash)) {
            fail(2, QStringLiteral("Duplicate pack decode_hash."));
        }
        hashes.insert(hash);
        records.append(record);
    }
    if (records.size() != static_cast<int>(number)) {
        fail(2, QStringLiteral("Pack record count does not match its header."));
    }
    return records;
}

class Connection final {
public:
    explicit Connection(const QString &path)
        : name(QUuid::createUuid().toString(QUuid::WithoutBraces))
        , db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name))
    {
        db.setDatabaseName(path);
    }
    ~Connection()
    {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    void open(bool readOnly)
    {
        db.setConnectOptions(readOnly ? QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000")
                                     : QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open()) {
            fail(4, QStringLiteral("Could not open feature store: %1").arg(db.lastError().text()));
        }
    }
    QString name;
    QSqlDatabase db;
};

void execute(QSqlQuery &query)
{
    if (!query.exec()) {
        fail(4, QStringLiteral("Feature pack SQL failed: %1").arg(query.lastError().text()));
    }
}

// A content group seen through its representative, the lexicographically first
// successful path. `record` is that file's own per-file analysis, so its scalars
// always describe the audio behind `hash`, whichever file filled the group row.
struct Group {
    qint64 id;
    QString hash;
    bool hasFeatures;
    bool hasAnalysis;
    bool validAnalysis;
    QJsonObject record;
};

QList<Group> loadGroups(QSqlDatabase db)
{
    QSqlQuery schema(db);
    schema.prepare(QStringLiteral("SELECT value FROM meta WHERE key = 'schema_version'"));
    execute(schema);
    if (!schema.next() || schema.value(0).toString() != QLatin1String("5")) {
        fail(2, QStringLiteral("Feature packs require schema 5; run refresh with this binary first."));
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT r.content_group_id, f.decode_hash, feat.content_group_id IS NOT NULL, ff.path IS NOT NULL, ff.extractor, ff.version, %1 "
        "FROM (SELECT content_group_id, MIN(path) AS path FROM files "
        "      WHERE status = 'ok' AND content_group_id IS NOT NULL GROUP BY content_group_id) r "
        "JOIN files f ON f.path = r.path "
        "LEFT JOIN features feat ON feat.content_group_id = r.content_group_id "
        "LEFT JOIN file_features ff ON ff.path = r.path "
        "ORDER BY f.decode_hash, r.content_group_id").arg(QStringLiteral("ff.") + scalarKeys.join(QStringLiteral(", ff."))));
    execute(query);
    QList<Group> groups;
    while (query.next()) {
        const QString hash = query.value(1).toString();
        bool valid = validHash(hash);
        QJsonObject scalars;
        for (qsizetype i = 0; i < scalarKeys.size(); ++i) {
            const QVariant value = query.value(static_cast<int>(i) + 6);
            QJsonValue scalar(QJsonValue::Null);
            if (!value.isNull()) {
                bool ok = false;
                const double number = value.toDouble(&ok);
                scalar = ok ? QJsonValue(number) : QJsonValue(QJsonValue::Undefined);
            }
            valid = valid && validScalar(scalarKeys.at(i), scalar);
            scalars.insert(scalarKeys.at(i), scalar);
        }
        groups.append(Group{query.value(0).toLongLong(), hash, query.value(2).toBool(), query.value(3).toBool(), valid,
            {{QStringLiteral("decode_hash"), hash}, {QStringLiteral("extractor"), query.value(4).toString()},
             {QStringLiteral("version"), query.value(5).toString()}, {QStringLiteral("scalars"), scalars}}});
    }
    return groups;
}

bool compatible(const QJsonObject &record)
{
    return record.value(QStringLiteral("extractor")) == QJsonValue(QStringLiteral("muzaiten-dsp"))
        && record.value(QStringLiteral("version")) == QJsonValue(QString::fromLatin1(Dsp::kDspVersion));
}

void writePack(const QString &path, const QList<QJsonObject> &records, bool replace, bool dryRun)
{
    if (records.size() > FeaturesPack::MaximumRecords) {
        fail(2, QStringLiteral("Feature pack exceeds 100000 records."));
    }
    const QFileInfo target(path);
    if (target.isSymLink() || (target.exists() && (!replace || !target.isFile()))) {
        fail(2, QStringLiteral("Export needs a regular file destination and --replace to overwrite it."));
    }
    QByteArray bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("format"), QStringLiteral("muzaiten-features-pack")},
        {QStringLiteral("version"), 1}, {QStringLiteral("records"), records.size()}}).toJson(QJsonDocument::Compact) + '\n';
    for (const QJsonObject &record : records) {
        validateRecord(record);
        const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (line.size() - 1 > FeaturesPack::MaximumLineBytes || bytes.size() + line.size() > FeaturesPack::MaximumBytes) {
            fail(2, QStringLiteral("Feature pack exceeds byte limits."));
        }
        bytes += line;
    }
    if (!dryRun) {
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            fail(4, QStringLiteral("Could not atomically write feature pack."));
        }
    }
}

void adopt(QSqlDatabase db, const QList<QPair<qint64, QJsonObject>> &rows)
{
    if (!db.transaction()) {
        fail(4, QStringLiteral("Could not begin feature pack import."));
    }
    try {
        QStringList updates;
        for (const QString &key : scalarKeys) {
            updates.append(key + QStringLiteral(" = excluded.") + key);
        }
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "INSERT INTO features(content_group_id, %1, extractor, version) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(content_group_id) DO UPDATE SET %2, extractor = excluded.extractor, version = excluded.version")
            .arg(scalarKeys.join(QStringLiteral(", ")), updates.join(QStringLiteral(", "))));
        for (const auto &row : rows) {
            query.bindValue(0, row.first);
            const QJsonObject scalars = row.second.value(QStringLiteral("scalars")).toObject();
            for (qsizetype i = 0; i < scalarKeys.size(); ++i) {
                const QJsonValue value = scalars.value(scalarKeys.at(i));
                query.bindValue(static_cast<int>(i) + 1, value.isNull() ? QVariant() : QVariant(value.toDouble()));
            }
            query.bindValue(10, row.second.value(QStringLiteral("extractor")).toString());
            query.bindValue(11, row.second.value(QStringLiteral("version")).toString());
            execute(query);
        }
        if (!db.commit()) {
            fail(4, QStringLiteral("Could not commit feature pack import."));
        }
    } catch (...) {
        db.rollback();
        throw;
    }
}

} // namespace

namespace FeaturesPack {

Error::Error(int code, const QString &message) : std::runtime_error(message.toStdString()), m_code(code) {}

QString contentsDescription()
{
    return QStringLiteral("Contains scalar audio features and extractor versions. Taste and listening history are absent, "
                          "but content hashes identify recordings. No paths, filenames, ratings, timestamps or embeddings.");
}

QJsonObject run(QStringList arguments)
{
    if (arguments.size() < 2) {
        fail(2, QStringLiteral("pack requires export|import FILE [--features PATH] [--dry-run] [--replace] [--json]."));
    }
    const QString operation = arguments.takeFirst();
    if (operation != QLatin1String("export") && operation != QLatin1String("import")) {
        fail(2, QStringLiteral("pack requires export or import."));
    }
    const QString path = arguments.takeFirst();
    if (path.isEmpty() || path.startsWith(u'-')) {
        fail(2, QStringLiteral("pack requires a local file path."));
    }
    QString featuresPath;
    bool dryRun = false;
    bool replace = false;
    bool json = false;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString option = arguments.at(i);
        if (option == QLatin1String("--features") && i + 1 < arguments.size()) {
            featuresPath = arguments.at(++i);
            if (featuresPath.isEmpty() || featuresPath.startsWith(u'-')) {
                fail(2, QStringLiteral("pack --features needs a path."));
            }
        } else if (option == QLatin1String("--dry-run")) {
            dryRun = true;
        } else if (option == QLatin1String("--replace")) {
            replace = true;
        } else if (option == QLatin1String("--json")) {
            json = true;
        } else {
            fail(2, QStringLiteral("Unknown pack option: %1.").arg(option));
        }
    }
    // Validate all input before even opening the store. Rejections cannot migrate it.
    const QList<QJsonObject> incoming = operation == QLatin1String("import") ? readPack(path) : QList<QJsonObject>{};
    if (featuresPath.isEmpty()) {
        featuresPath = QDir(AppPaths::dataDir()).filePath(QStringLiteral("features.sqlite"));
    }
    const QFileInfo store(featuresPath);
    if (!store.isFile()) {
        fail(4, QStringLiteral("Feature store is absent; run refresh first."));
    }
    const QString storePath = store.canonicalFilePath();
    const QFileInfo pack(path);
    const QString packPath = QDir(pack.dir().canonicalPath()).filePath(pack.fileName());
    for (const QString &suffix : {QString(), QStringLiteral(".lock"), QStringLiteral("-wal"),
                                 QStringLiteral("-shm"), QStringLiteral("-journal")}) {
        if (operation == QLatin1String("export")
            && (packPath == storePath + suffix || pack.canonicalFilePath() == storePath + suffix)) {
            fail(2, QStringLiteral("Export destination must not be the feature store or its sidecars."));
        }
    }
    QLockFile lock(featuresPath + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) {
        fail(5, QStringLiteral("Feature store is busy."));
    }
    Connection connection(featuresPath);
    connection.open(true);
    const QList<Group> groups = loadGroups(connection.db);
    QJsonObject report{{QStringLiteral("format"), QStringLiteral("muzaiten-features-pack")},
                       {QStringLiteral("version"), 1}, {QStringLiteral("operation"), operation},
                       {QStringLiteral("dry_run"), dryRun}, {QStringLiteral("replace"), replace},
                       {QStringLiteral("contents"), contentsDescription()}};
    int skipped = 0;
    int mismatched = 0;
    int incompatible = 0;
    int existing = 0;
    int invalid = 0;
    int adopted = 0;
    if (operation == QLatin1String("export")) {
        QList<QJsonObject> records;
        QSet<QString> hashes;
        for (const Group &group : groups) {
            if (!group.hasAnalysis || !compatible(group.record)) {
                ++skipped;
                continue;
            }
            if (!group.validAnalysis) {
                ++invalid;
                ++skipped;
                continue;
            }
            if (hashes.contains(group.hash)) {
                fail(4, QStringLiteral("Feature store has ambiguous representative hashes; run refresh first."));
            }
            hashes.insert(group.hash);
            records.append(group.record);
        }
        writePack(path, records, replace, dryRun);
        report.insert(QStringLiteral("records"), records.size());
        report.insert(QStringLiteral("exported"), records.size());
        report.insert(QStringLiteral("invalid"), invalid);
    } else {
        QHash<QString, QList<Group>> byHash;
        for (const Group &group : groups) {
            byHash[group.hash].append(group);
        }
        QList<QPair<qint64, QJsonObject>> rows;
        QSet<qint64> matchingGroups;
        QSet<QString> compatibleHashes;
        for (const QJsonObject &record : incoming) {
            if (!compatible(record)) {
                ++incompatible;
                ++mismatched;
                continue;
            }
            const QString hash = record.value(QStringLiteral("decode_hash")).toString();
            compatibleHashes.insert(hash);
            const QList<Group> matches = byHash.value(hash);
            if (matches.size() != 1) {
                ++mismatched;
                continue;
            }
            const Group &group = matches.first();
            matchingGroups.insert(group.id);
            if (group.hasFeatures && !replace) {
                ++existing;
                ++skipped;
                continue;
            }
            rows.append({group.id, record});
        }
        adopted = static_cast<int>(rows.size());
        report.insert(QStringLiteral("records"), incoming.size());
        report.insert(QStringLiteral("destination_groups"), groups.size());
        report.insert(QStringLiteral("destination_unmatched_groups"), groups.size() - matchingGroups.size());
        QSqlQuery files(connection.db);
        files.prepare(QStringLiteral("SELECT decode_hash FROM files"));
        execute(files);
        int fileCount = 0;
        int unmatchedFiles = 0;
        while (files.next()) {
            ++fileCount;
            if (!compatibleHashes.contains(files.value(0).toString())) {
                ++unmatchedFiles;
            }
        }
        report.insert(QStringLiteral("destination_files"), fileCount);
        report.insert(QStringLiteral("destination_unmatched_files"), unmatchedFiles);
        if (!dryRun && !rows.isEmpty()) {
            files.finish();
            connection.db.close();
            connection.open(false);
            adopt(connection.db, rows);
        }
    }
    report.insert(QStringLiteral("adopted"), adopted);
    report.insert(QStringLiteral("skipped"), skipped);
    report.insert(QStringLiteral("mismatched"), mismatched);
    report.insert(QStringLiteral("incompatible"), incompatible);
    report.insert(QStringLiteral("existing"), existing);
    if (json) {
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    } else {
        QTextStream out(stdout);
        out << contentsDescription() << '\n' << operation << (dryRun ? " preview" : "")
            << ": records=" << report.value(QStringLiteral("records")).toInt()
            << " adopted=" << adopted << " skipped=" << skipped << " mismatched=" << mismatched
            << " incompatible=" << incompatible << " existing=" << existing;
        if (operation == QLatin1String("export")) {
            out << " invalid=" << invalid;
        }
        out << '\n';
        if (operation == QLatin1String("import")) {
            out << "Destination: groups=" << groups.size()
                << " unmatched_groups=" << report.value(QStringLiteral("destination_unmatched_groups")).toInt()
                << " files=" << report.value(QStringLiteral("destination_files")).toInt()
                << " unmatched_files=" << report.value(QStringLiteral("destination_unmatched_files")).toInt() << '\n';
        }
    }
    return report;
}

} // namespace FeaturesPack
