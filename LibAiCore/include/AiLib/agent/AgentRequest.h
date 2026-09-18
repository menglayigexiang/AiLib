#pragma once

#include <AiLib/agent/AgentLimits.h>
#include <AiLib/agent/AgentEvent.h>
#include <AiLib/client/ChatRequest.h>
#include <QStringList>

namespace AiLib {
// 保存本轮输入和执行配置，不拥有长期历史或注册表。
struct AgentRequest {
    ChatRequest chat;               // 本次运行的模型参数及调用方历史
    RequestOptions requestOptions;  // 本次请求选项，提供唯一取消来源
    AgentLimits limits;             // 本次运行的轮数、执行次数及时间限制
    // 严格白名单：空表示无工具，定义只来自 Registry。
    QStringList enabledTools;  // 严格工具名称白名单，空表示不启用任何工具
    AgentCallback callback;    // 在运行线程同步接收工具执行事件，不调度工具
};
}  // AiLib 命名空间结束
