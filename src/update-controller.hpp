#pragma once
#include "plugin-updater.hpp"
#include <QSettings>

class QWidget;
namespace livehime {
class UpdateController final : public QObject {
    Q_OBJECT
  public:
    UpdateController(const QString &configDir, const QString &cacheDir, const QString &helper, QObject *parent = nullptr);
    QWidget *createControls(QWidget *parent);
    void onObsExit();
  signals:
    void changed();
    void updateFailed(const QString &detail);
  private:
    void check();
    void onUpdateChanged();
    bool installOnExit() const;
    PluginUpdater updater_;
    QSettings settings_;
    QString cacheDir_, helper_;
    bool automatic_, betaUpdates_, downloadAttempted_ = false, installAtExit_, exiting_ = false;
};
}
