#pragma once

#include <AiLib/Export.h>
#include <AiLib/stream/StreamEvent.h>
#include <QMap>
#include <QSet>

namespace AiLib {
// 聚合一次请求的标准化事件，保留有效内容，不理解任何厂商协议字段。
class AILIB_EXPORT StreamSession {
public:
    StreamSession();                                        // 初始化未完成的助手响应
    bool apply(const StreamEvent& event, SdkError& error);  // 校验事件关系并聚合内容，输出错误
    ChatResponse response() const;                          // 生成当前值快照，未完成工具不进入消息
    void fail(const SdkError& error);                       // 保留已有数据，将最终状态标记为未完成
    const SdkError& error() const;                          // 查询本请求最后的流程故障

private:
    // 保存逻辑内容块的聚合状态，工具原始参数只存在于 Session 内。
    struct Part {
        StreamPartType type = StreamPartType::Text;  // 标准化的逻辑内容类别
        QString text;                                // 累积文本、推理或工具参数 JSON
        QString callId;                              // 已收到的工具调用 ID 属性
        QString toolName;                            // 累积的函数名称
        bool completed = false;                      // 是否收到 PartCompleted
        bool argumentsValid = false;                 // 工具参数是否完整合法，Length 时允许丢弃截断参数
        QJsonObject arguments;                       // 完整且合法的工具参数对象
    };
    ChatResponse m_response;  // 响应级数据及协议完整性
    SdkError m_error;         // 本次请求故障，不混入工具业务结果
    QMap<int, Part> m_parts;  // 逻辑索引到内容聚合状态的映射
    QList<int> m_order;       // 按首次出现顺序保存内容块顺序
    QSet<QString> m_callIds;  // 已完成工具调用的响应内唯一 ID 集合
};
}  // AiLib 命名空间结束
