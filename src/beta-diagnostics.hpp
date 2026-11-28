#pragma once

#if LIVEHIME_BETA_BUILD
#include <QJsonObject>
#include <QString>
namespace livehime::diagnostics {
void start(const QString &configDir);
void record(const QString &event, QJsonObject fields = {});
}
#else
#include <QString>
namespace livehime::diagnostics {
inline void start(const QString &) {}
}
#endif
