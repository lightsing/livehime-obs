#pragma once
#include <QJsonObject>

namespace livehime::diagnostics {
// Writes the completed archive atomically. An empty return value indicates success.
QString exportArchive(const QString &destination, QJsonObject report);
}
