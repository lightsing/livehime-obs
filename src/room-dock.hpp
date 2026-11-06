#pragma once
#include "live-controller.hpp"
#include <QWidget>
#include <QPointer>
#include <QPixmap>
#include <QTemporaryDir>

class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QResizeEvent;

namespace livehime {
class LoginDialog;
class CoverButton;
class NativeStreamControls;
class RoomDock final : public QWidget {
    Q_OBJECT
public:
    explicit RoomDock(const QString &storageDirectory, QWidget *parent = nullptr);
    bool attachNativeControls(QWidget *mainWindow, QString *error);
private:
    void updateView();
    void updateNativeControls();
    bool canStart() const;
    void nativeStart();
    void nativeStop();
    void updateRoom();
    void login();
    void selectCover();
    void updateAreas();
    void updateChildAreas(int selection = 0);
    void updateCover();
    void updateTitle();
    void showCover();
    void resizeEvent(QResizeEvent *event) override;
    int selectedAreaId() const;
    LiveController *controller_;
    NativeStreamControls *nativeControls_;
    QLabel *account_;
    QLabel *roomLabel_;
    QLabel *status_;
    CoverButton *coverPreview_;
    QLabel *titleHint_;
    QLabel *review_;
    QLineEdit *title_;
    QComboBox *parentArea_;
    QComboBox *area_;
    QLabel *areaHint_;
    QPushButton *login_;
    QPushButton *logout_;
    QPushButton *refresh_;
    QPushButton *retry_;
    QPushButton *verification_;
    QTemporaryDir coverDirectory_;
    QPointer<LoginDialog> loginDialog_;
    QPixmap currentCover_;
    bool areaEdited_ = false;
    bool nativeControlsAttached_ = false;
    qint64 displayedRoom_ = 0;
};
}
