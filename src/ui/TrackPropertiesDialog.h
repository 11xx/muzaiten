#pragma once

#include "core/MetadataBlob.h"
#include "core/Track.h"

#include <QDialog>

class QTextBrowser;

// Read-only "File properties"-style view of a single track: the promoted
// Track fields (file, audio, library) plus every captured tag from the full
// metadata archive, pretty-printed. DB-agnostic — the caller resolves the
// FullMetadata (e.g. via Database::fullMetadata) and hands both in.
class TrackPropertiesDialog final : public QDialog {
    Q_OBJECT
public:
    explicit TrackPropertiesDialog(const Track &track,
                                   const MetadataBlob::FullMetadata &metadata,
                                   QWidget *parent = nullptr);

    // Optional extra section appended after the tag list — used by the playlist
    // view to show playlist-specific data (added/modified timestamps, comment,
    // remembered query) for the item. Pass label/value pairs as HTML-safe text.
    void setExtraSection(const QString &title, const QVector<QPair<QString, QString>> &rows);

private:
    void rebuild();

    Track m_track;
    MetadataBlob::FullMetadata m_metadata;
    QString m_extraTitle;
    QVector<QPair<QString, QString>> m_extraRows;
    QTextBrowser *m_view = nullptr;
};
