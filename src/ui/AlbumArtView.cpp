#include "ui/AlbumArtView.h"

#include <QFrame>
#include <QIcon>
#include <QImageReader>
#include <QPixmap>
#include <QResizeEvent>

AlbumArtView::AlbumArtView(QWidget *parent)
    : QLabel(parent)
{
    setAlignment(Qt::AlignCenter);
    setFrameShape(QFrame::StyledPanel);
    setScaledContents(false);
}

void AlbumArtView::setSourcePath(const QString &path)
{
    m_sourcePath = path;
    m_sourceImage = QImage();
    updateScaledPixmap();
}

void AlbumArtView::setSourceImage(const QImage &image)
{
    m_sourceImage = image;
    m_sourcePath.clear();
    updateScaledPixmap();
}

void AlbumArtView::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    updateScaledPixmap();
}

void AlbumArtView::updateScaledPixmap()
{
    const QSize target = contentsRect().size();
    if (target.width() <= 0 || target.height() <= 0) {
        setPixmap({});
        return;
    }

    const qreal dpr = devicePixelRatioF();
    const QSize pxTarget(qRound(target.width() * dpr), qRound(target.height() * dpr));

    auto applyRaster = [&](const QImage &source) {
        if (source.isNull()) {
            return false;
        }
        QPixmap scaled = QPixmap::fromImage(source.scaled(pxTarget, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        scaled.setDevicePixelRatio(dpr);
        setPixmap(scaled);
        return true;
    };

    if (!m_sourceImage.isNull()) {
        applyRaster(m_sourceImage);
        return;
    }

    if (m_sourcePath.isEmpty()) {
        setPixmap({});
        return;
    }

    if (m_sourcePath.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
        QIcon icon(m_sourcePath);
        if (icon.isNull()) {
            setPixmap({});
            return;
        }
        setPixmap(icon.pixmap(target, dpr));
        return;
    }

    QImageReader reader(m_sourcePath);
    if (!reader.canRead()) {
        setPixmap({});
        return;
    }

    const QSize sourceSize = reader.size();
    if (sourceSize.isValid() && sourceSize.width() > 0 && sourceSize.height() > 0) {
        reader.setScaledSize(sourceSize.scaled(pxTarget, Qt::KeepAspectRatio));
    }
    applyRaster(reader.read());
}
