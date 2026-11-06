#pragma once

// A real Qt signal graph shaped like OBS 33's controls, with no OBS dependency.
#include <QAction>
#include <QFrame>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace test_support {

class OBSBasicControls final : public QFrame {
    Q_OBJECT
public:
    explicit OBSBasicControls(QWidget *parent = nullptr) : QFrame(parent)
    {
        setObjectName(QStringLiteral("OBSBasicControls"));
        auto *layout = new QVBoxLayout(this);
        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("controlsFrame"));
        auto *buttons = new QVBoxLayout(frame);
        streamButton = new QPushButton(QStringLiteral("Start Streaming"), frame);
        streamButton->setObjectName(QStringLiteral("streamButton"));
        streamButton->setToolTip(QStringLiteral("Native streaming control"));
        buttons->addWidget(streamButton);
        layout->addWidget(frame);
        streamMenu = new QMenu(this);
        streamMenu->setObjectName(QStringLiteral("nativeStreamMenu"));
        // Preserve the exact action order from OBSBasicControls.cpp.
        startStreamAction = streamMenu->addAction(QStringLiteral("Start Streaming"));
        stopStreamAction = streamMenu->addAction(QStringLiteral("Stop Streaming"));
        forceStopStreamAction = streamMenu->addAction(QStringLiteral("Force Stop Streaming"));
        connect(streamButton, &QPushButton::clicked, this,
                [this] { emit StreamButtonClicked(); }, Qt::DirectConnection);
        connect(startStreamAction, &QAction::triggered, this,
                [this] { emit StartStreamMenuActionClicked(); }, Qt::DirectConnection);
        connect(stopStreamAction, &QAction::triggered, this,
                [this] { emit StopStreamMenuActionClicked(); }, Qt::DirectConnection);
        connect(forceStopStreamAction, &QAction::triggered, this,
                [this] { emit ForceStopStreamMenuActionClicked(); }, Qt::DirectConnection);
    }

    QPushButton *streamButton = nullptr;
    QMenu *streamMenu = nullptr;
    QAction *startStreamAction = nullptr;
    QAction *stopStreamAction = nullptr;
    QAction *forceStopStreamAction = nullptr;

public slots:
    void StreamingPreparing()
    {
        streamButton->setEnabled(false);
        streamButton->setText(QStringLiteral("Preparing native stream"));
    }
    void StreamingStarting(bool broadcastAutoStart)
    {
        Q_UNUSED(broadcastAutoStart);
        streamButton->setText(QStringLiteral("Connecting native stream"));
    }
    void StreamingStarted(bool withDelay)
    {
        streamButton->setEnabled(true);
        streamButton->setText(QStringLiteral("Stop Streaming"));
        streamButton->setProperty("class", QStringLiteral("state-active"));
        if (withDelay) {
            streamButton->setMenu(streamMenu);
            startStreamAction->setVisible(false);
            stopStreamAction->setVisible(true);
        }
    }
    void StreamingStopping()
    {
        streamButton->setText(QStringLiteral("Stopping native stream"));
    }
    void StreamingStopped(bool withDelay)
    {
        streamButton->setEnabled(true);
        streamButton->setText(QStringLiteral("Start Streaming"));
        streamButton->setProperty("class", QString{});
        if (withDelay) {
            if (!streamButton->menu()) streamButton->setMenu(streamMenu);
            startStreamAction->setVisible(true);
            stopStreamAction->setVisible(false);
        } else streamButton->setMenu(nullptr);
    }

signals:
    void StreamButtonClicked();
    void StartStreamMenuActionClicked();
    void StopStreamMenuActionClicked();
    void ForceStopStreamMenuActionClicked();
};

class NativeControlsWindow final : public QWidget {
    Q_OBJECT
public:
    explicit NativeControlsWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("OBSBasic"));
        setWindowTitle(QStringLiteral("Offline native controls fixture"));
        auto *layout = new QVBoxLayout(this);
        controls = new OBSBasicControls(this);
        streamButton = controls->streamButton;
        layout->addWidget(controls);
        connect(controls, &OBSBasicControls::StreamButtonClicked, this,
                &NativeControlsWindow::StreamActionTriggered);
        connect(controls, &OBSBasicControls::StartStreamMenuActionClicked, this,
                &NativeControlsWindow::StartStreaming);
        connect(controls, &OBSBasicControls::StopStreamMenuActionClicked, this,
                &NativeControlsWindow::StopStreaming);
        connect(controls, &OBSBasicControls::ForceStopStreamMenuActionClicked, this,
                &NativeControlsWindow::ForceStopStreaming);
        connect(this, &NativeControlsWindow::StreamingPreparing, controls,
                &OBSBasicControls::StreamingPreparing);
        connect(this, &NativeControlsWindow::StreamingStarting, controls,
                &OBSBasicControls::StreamingStarting);
        connect(this, &NativeControlsWindow::StreamingStarted, controls,
                &OBSBasicControls::StreamingStarted);
        connect(this, &NativeControlsWindow::StreamingStopping, controls,
                &OBSBasicControls::StreamingStopping);
        connect(this, &NativeControlsWindow::StreamingStopped, controls,
                &OBSBasicControls::StreamingStopped);
    }

    OBSBasicControls *controls = nullptr;
    QPushButton *streamButton = nullptr;
    int originalStarts = 0;
    int originalStops = 0;
    int originalClicks = 0;
    int forceStops = 0;

public slots:
    void StreamActionTriggered() { ++originalClicks; }
    void StartStreaming() { ++originalStarts; }
    void StopStreaming() { ++originalStops; }
    void ForceStopStreaming() { ++forceStops; }

signals:
    void StreamingPreparing();
    void StreamingStarting(bool broadcastAutoStart);
    void StreamingStarted(bool withDelay);
    void StreamingStopping();
    void StreamingStopped(bool withDelay);
};
} // namespace test_support
