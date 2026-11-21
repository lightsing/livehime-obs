#pragma once

#include <QScrollArea>
#include <QVBoxLayout>

namespace livehime {
class DockScrollContent final : public QWidget {
  public:
    int heightForWidth(int width) const override
    {
        // QScrollArea treats this height as a minimum. Wrap the labels, but let lists shrink.
        return layout()->totalMinimumHeightForWidth(width);
    }
};

inline QVBoxLayout *addScrollableDockContent(QVBoxLayout *shell)
{
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new DockScrollContent;
    // Keep the content as wide as the viewport, even if the OBS theme gives buttons a wider size hint.
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    content->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->addWidget(scroll);
    return layout;
}
} // namespace livehime
