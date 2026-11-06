#include "cover-crop-dialog.hpp"

#include <QColorSpace>
#include <QDialogButtonBox>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace livehime {
namespace {
constexpr double MaximumZoom = 8.0;
constexpr int SliderSteps = 1000;
constexpr int OutputWidth = 720;
constexpr int OutputHeight = 540;
constexpr qint64 MaximumPixels = 80000000;
constexpr int MaximumDimension = 32768;

double zoomForSlider(int value)
{
    return std::pow(MaximumZoom, static_cast<double>(value) / SliderSteps);
}
int sliderForZoom(double zoom)
{
    return std::clamp(qRound(std::log(zoom) / std::log(MaximumZoom) * SliderSteps), 0, SliderSteps);
}
} // namespace

class CoverCropCanvas final : public QWidget {
public:
    explicit CoverCropCanvas(const QImage &image, QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("coverCropCanvas"));
        setMinimumSize(360, 260);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAccessibleName(QStringLiteral("封面裁切预览"));
        setAccessibleDescription(QStringLiteral("固定四比三裁切框。拖动图片调整位置，滚轮缩放，方向键微调，Home 键重置。"));
        if (image.isNull()) error_ = QStringLiteral("无法读取这张图片，请重新选择。");
        else if (image.width() > MaximumDimension || image.height() > MaximumDimension ||
                 static_cast<qint64>(image.width()) * image.height() > MaximumPixels)
            error_ = QStringLiteral("图片过大，请选择不超过 8000 万像素、单边不超过 32768 像素的图片。");
        else {
            source_ = image;
            if (source_.devicePixelRatio() != 1.0) source_.setDevicePixelRatio(1.0);
            center_ = QPointF(source_.width() * 0.5, source_.height() * 0.5);
            // The original is retained for output; a bounded image makes fitting
            // large photos responsive. Close-up painting uses original pixels.
            const double factor = std::min(1.0, 2560.0 / std::max(source_.width(), source_.height()));
            if (factor < 1.0) {
                const QSize size(std::max(1, qRound(source_.width() * factor)),
                                 std::max(1, qRound(source_.height() * factor)));
                preview_ = source_.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            } else preview_ = source_;
            // Convert only the bounded preview for accurate display. Export color
            // conversion happens after cropping to 720 x 540, not on the full photo.
            if (preview_.colorSpace().isValid() && preview_.colorSpace() != QColorSpace(QColorSpace::SRgb))
                preview_ = preview_.convertedToColorSpace(QColorSpace::SRgb);
        }
        setCursor(valid() ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }

    bool valid() const { return !source_.isNull(); }
    double zoom() const { return zoom_; }
    std::function<void(double)> zoomChanged;

    void setZoom(double value, std::optional<QPointF> anchor = {})
    {
        if (!valid()) return;
        value = std::clamp(value, 1.0, MaximumZoom);
        if (qFuzzyCompare(value, zoom_)) return;
        const auto frame = cropFrame();
        const double before = imageScale();
        std::optional<QPointF> anchoredPixel;
        if (anchor && frame.contains(*anchor)) anchoredPixel = center_ + (*anchor - frame.center()) / before;
        zoom_ = value;
        if (anchoredPixel) center_ = *anchoredPixel - (*anchor - frame.center()) / imageScale();
        clampCenter();
        update();
        if (zoomChanged) zoomChanged(zoom_);
    }

    void reset()
    {
        if (!valid()) return;
        zoom_ = 1.0;
        center_ = QPointF(source_.width() * 0.5, source_.height() * 0.5);
        clampCenter();
        update();
        if (zoomChanged) zoomChanged(zoom_);
    }

    QImage croppedImage() const
    {
        if (!valid()) return {};
        const auto frame = cropFrame();
        const double scale = imageScale();
        const QSizeF extent(frame.width() / scale, frame.height() / scale);
        const QRectF sourceRect(center_ - QPointF(extent.width() * 0.5, extent.height() * 0.5), extent);
        QImage output(OutputWidth, OutputHeight, QImage::Format_RGB32);
        output.fill(Qt::white);
        {
            QPainter painter(&output);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRectF(0, 0, OutputWidth, OutputHeight), source_, sourceRect);
        }
        if (source_.colorSpace().isValid()) {
            output.setColorSpace(source_.colorSpace());
            if (output.colorSpace() != QColorSpace(QColorSpace::SRgb))
                output = output.convertedToColorSpace(QColorSpace::SRgb);
        } else output.setColorSpace(QColorSpace::SRgb);
        return output;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), QColor(24, 26, 31));
        if (!valid()) {
            painter.setPen(QColor(220, 222, 228));
            painter.drawText(rect().adjusted(28, 28, -28, -28), Qt::AlignCenter | Qt::TextWordWrap, error_);
            return;
        }
        const auto frame = cropFrame();
        const double scale = imageScale();
        const QRectF imageRect(frame.center() - center_ * scale,
                               QSizeF(source_.width() * scale, source_.height() * scale));
        const auto visible = imageRect.intersected(QRectF(rect()));
        // Intersect before painting so even a one-pixel-tall panorama never sends
        // huge off-screen raster coordinates to QPainter.
        const bool sourceIsSrgb = !source_.colorSpace().isValid() || source_.colorSpace() == QColorSpace(QColorSpace::SRgb);
        const QImage &display = scale >= 0.25 && sourceIsSrgb ? source_ : preview_;
        const QRectF sample((visible.left() - imageRect.left()) / imageRect.width() * display.width(),
                            (visible.top() - imageRect.top()) / imageRect.height() * display.height(),
                            visible.width() / imageRect.width() * display.width(),
                            visible.height() / imageRect.height() * display.height());
        painter.fillRect(visible, Qt::white);
        painter.drawImage(visible, display, sample);

        QPainterPath shade;
        shade.setFillRule(Qt::OddEvenFill);
        shade.addRect(QRectF(rect()));
        shade.addRect(frame);
        painter.fillPath(shade, QColor(0, 0, 0, 155));

        painter.setPen(QPen(QColor(255, 255, 255, dragging_ ? 125 : 75), 1.0));
        for (int i = 1; i <= 2; ++i) {
            const double x = frame.left() + frame.width() * i / 3.0;
            const double y = frame.top() + frame.height() * i / 3.0;
            painter.drawLine(QPointF(x, frame.top()), QPointF(x, frame.bottom()));
            painter.drawLine(QPointF(frame.left(), y), QPointF(frame.right(), y));
        }
        painter.setPen(QPen(QColor(255, 255, 255, 230), 1.25));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(frame);
        painter.setPen(QPen(Qt::white, 3.0, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        constexpr double corner = 18.0;
        for (const auto &point : {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight()}) {
            const double dx = point.x() < frame.center().x() ? corner : -corner;
            const double dy = point.y() < frame.center().y() ? corner : -corner;
            painter.drawLine(point, point + QPointF(dx, 0));
            painter.drawLine(point, point + QPointF(0, dy));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (!valid() || event->button() != Qt::LeftButton) { QWidget::mousePressEvent(event); return; }
        setFocus(Qt::MouseFocusReason);
        dragging_ = true;
        lastPosition_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        update();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging_) { QWidget::mouseMoveEvent(event); return; }
        panBy(event->position() - lastPosition_);
        lastPosition_ = event->position();
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            setCursor(Qt::OpenHandCursor);
            update();
            event->accept();
        } else QWidget::mouseReleaseEvent(event);
    }
    void wheelEvent(QWheelEvent *event) override
    {
        if (!valid()) { event->ignore(); return; }
        double steps = event->angleDelta().y() / 120.0;
        if (qFuzzyIsNull(steps) && !event->pixelDelta().isNull()) steps = event->pixelDelta().y() / 100.0;
        if (qFuzzyIsNull(steps)) { event->ignore(); return; }
        setZoom(zoom_ * std::pow(1.15, steps), event->position());
        event->accept();
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 40.0 : 8.0;
        switch (event->key()) {
        case Qt::Key_Left: panBy(QPointF(-step, 0)); break;
        case Qt::Key_Right: panBy(QPointF(step, 0)); break;
        case Qt::Key_Up: panBy(QPointF(0, -step)); break;
        case Qt::Key_Down: panBy(QPointF(0, step)); break;
        case Qt::Key_Plus: case Qt::Key_Equal: setZoom(zoom_ * 1.1); break;
        case Qt::Key_Minus: setZoom(zoom_ / 1.1); break;
        case Qt::Key_Home: reset(); break;
        default: QWidget::keyPressEvent(event); return;
        }
        event->accept();
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        dragging_ = false;
        setCursor(valid() ? Qt::OpenHandCursor : Qt::ArrowCursor);
        update();
        QWidget::focusOutEvent(event);
    }
    void resizeEvent(QResizeEvent *event) override
    {
        clampCenter();
        QWidget::resizeEvent(event);
    }

private:
    QRectF cropFrame() const
    {
        constexpr double padding = 28.0;
        const double availableWidth = std::max(1.0, width() - 2.0 * padding);
        const double availableHeight = std::max(1.0, height() - 2.0 * padding);
        const double frameWidth = std::min(availableWidth, availableHeight * 4.0 / 3.0);
        const QSizeF size(frameWidth, frameWidth * 3.0 / 4.0);
        return QRectF(QPointF((width() - size.width()) * 0.5, (height() - size.height()) * 0.5), size);
    }
    double imageScale() const
    {
        if (!valid()) return 1.0;
        const auto frame = cropFrame();
        return std::max(frame.width() / source_.width(), frame.height() / source_.height()) * zoom_;
    }
    void clampCenter()
    {
        if (!valid()) return;
        const auto frame = cropFrame();
        const double scale = imageScale();
        const double halfWidth = std::min(source_.width() * 0.5, frame.width() / (2.0 * scale));
        const double halfHeight = std::min(source_.height() * 0.5, frame.height() / (2.0 * scale));
        center_.setX(std::clamp(center_.x(), halfWidth, source_.width() - halfWidth));
        center_.setY(std::clamp(center_.y(), halfHeight, source_.height() - halfHeight));
    }
    void panBy(const QPointF &pixels)
    {
        if (!valid()) return;
        center_ -= pixels / imageScale();
        clampCenter();
        update();
    }

    QImage source_;
    QImage preview_;
    QString error_;
    QPointF center_;
    QPointF lastPosition_;
    double zoom_ = 1.0;
    bool dragging_ = false;
};

CoverCropDialog::CoverCropDialog(const QImage &image, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("coverCropDialog"));
    setWindowTitle(QStringLiteral("调整封面"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setModal(true);
    setSizeGripEnabled(true);
    setMinimumSize(600, 470);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    auto *instructions = new QLabel(QStringLiteral("拖动图片调整位置，滚轮或滑块缩放。"), this);
    instructions->setTextFormat(Qt::PlainText);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    canvas_ = new CoverCropCanvas(image, this);
    layout->addWidget(canvas_, 1);

    auto *zoomRow = new QHBoxLayout;
    zoomRow->setSpacing(10);
    auto *zoomLabel = new QLabel(QStringLiteral("缩放"), this);
    zoomRow->addWidget(zoomLabel);
    auto *slider = new QSlider(Qt::Horizontal, this);
    slider->setObjectName(QStringLiteral("coverZoomSlider"));
    slider->setRange(0, SliderSteps);
    slider->setSingleStep(15);
    slider->setPageStep(80);
    slider->setTracking(true);
    slider->setAccessibleName(QStringLiteral("封面缩放"));
    zoomLabel->setBuddy(slider);
    zoomRow->addWidget(slider, 1);
    auto *percentage = new QLabel(QStringLiteral("100%"), this);
    percentage->setMinimumWidth(52);
    percentage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    zoomRow->addWidget(percentage);
    auto *reset = new QPushButton(QStringLiteral("重置"), this);
    reset->setObjectName(QStringLiteral("coverCropReset"));
    reset->setAutoDefault(false);
    zoomRow->addWidget(reset);
    layout->addLayout(zoomRow);
    auto *details = new QLabel(QStringLiteral("固定 4:3 · 保存为 720 × 540"), this);
    details->setTextFormat(Qt::PlainText);
    layout->addWidget(details);

    auto *buttons = new QDialogButtonBox(this);
    auto *cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    cancel->setObjectName(QStringLiteral("coverCropCancel"));
    auto *save = buttons->addButton(QStringLiteral("保存并上传"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("coverCropSave"));
    save->setDefault(true);
    layout->addWidget(buttons);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, [this] {
        result_ = canvas_->croppedImage();
        if (!result_.isNull()) accept();
    });
    connect(slider, &QSlider::valueChanged, this, [this](int value) { canvas_->setZoom(zoomForSlider(value)); });
    connect(reset, &QPushButton::clicked, canvas_, [this] { canvas_->reset(); });
    canvas_->zoomChanged = [slider, percentage](double value) {
        const QSignalBlocker blocker(slider);
        slider->setValue(sliderForZoom(value));
        percentage->setText(QString::number(qRound(value * 100.0)) + '%');
    };
    slider->setEnabled(canvas_->valid());
    reset->setEnabled(canvas_->valid());
    save->setEnabled(canvas_->valid());

    const auto *availableScreen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QSize available = availableScreen ? availableScreen->availableGeometry().size() : QSize(1280, 800);
    resize(std::min(860, std::max(600, available.width() - 80)),
           std::min(680, std::max(470, available.height() - 80)));
}

QImage CoverCropDialog::croppedImage() const { return result_; }
} // namespace livehime
