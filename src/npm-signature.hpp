#pragma once
#include <QJsonObject>

namespace livehime {
bool trustedMirrorSignature(const QString &name, const QString &version, const QJsonObject &dist);
}
