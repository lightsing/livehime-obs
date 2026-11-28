#pragma once
#include "protocol.hpp"

namespace livehime {
struct PluginIssue {
    QString operation;
    QString detail;
    QJsonObject codes{};
    // Only plugin-authored, non-personal messages may be copied into a report.
    QString exportReason{};
    static PluginIssue local(const QString &operation, const QString &reason) {
        return {operation, reason, {}, reason};
    }
    static PluginIssue api(const QString &operation, const ApiError &error, const QString &detail = {}) {
        return {operation, detail.isEmpty() ? error.message : detail,
                {{"kind", int(error.kind)}, {"api", error.code}, {"http", error.httpStatus},
                 {"network", error.networkCode}, {"outcome_unknown", error.outcomeUnknown}}};
    }
};
}
