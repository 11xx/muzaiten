#pragma once

#include <QImage>
#include <QLabel>

class AlbumArtView final : public QLabel {
    Q_OBJECT

public:
    explicit AlbumArtView(QWidget *parent = nullptr);

    void setSourcePath(const QString &path);
    void setSourceImage(const QImage &image);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateScaledPixmap();

    QString m_sourcePath;
    QImage m_sourceImage;
};
