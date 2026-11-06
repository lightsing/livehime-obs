#pragma once

#include <QDialog>
#include <QImage>

namespace livehime {
class CoverCropCanvas;

class CoverCropDialog final : public QDialog {
    Q_OBJECT
public:
    // The caller supplies an image with EXIF orientation already applied.
    explicit CoverCropDialog(const QImage &image, QWidget *parent = nullptr);
    // Empty until the user accepts; otherwise a white-backed 720 x 540 crop.
    QImage croppedImage() const;

private:
    CoverCropCanvas *canvas_ = nullptr;
    QImage result_;
};
} // namespace livehime
