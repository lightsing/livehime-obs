#pragma once

class QDockWidget;
class QMainWindow;

namespace livehime {
void installDockFocusPolicy(QDockWidget *dock, QMainWindow *host);
}
