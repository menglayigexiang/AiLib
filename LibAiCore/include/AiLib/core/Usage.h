#pragma once

#include <QtCore/qglobal.h>
#include <optional>

namespace AiLib {
// nullopt 表示未报告，零表示服务端明确报告为零。
// 任何参与汇总轮次字段未知时，对应完整总量也未知。
struct Usage {
    std::optional<qint64> inputTokens;   // 输入 Token 数，未知用 nullopt，零表示已报告为零
    std::optional<qint64> outputTokens;  // 输出 Token 数，未知用 nullopt，零表示已报告为零
    std::optional<qint64> totalTokens;   // 服务端报告的总 Token 数，不自行推算
};
}  // AiLib 命名空间结束
