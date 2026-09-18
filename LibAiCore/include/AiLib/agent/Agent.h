#pragma once

#include <AiLib/agent/AgentRequest.h>
#include <AiLib/agent/AgentResult.h>
#include <AiLib/client/LLMClient.h>
#include <AiLib/tools/ToolExecutor.h>
#include <atomic>
#include <memory>

namespace AiLib {
// 同步协调模型与客户端工具执行；拥有 Client，历史和 Registry 由应用管理。
class AILIB_EXPORT Agent {
public:
    Agent(std::unique_ptr<LLMClient> client,                  // 转移模型客户端所有权
          ToolRegistry& registry,                             // 使用应用拥有的注册表
          IToolApprovalProvider* approvalProvider = nullptr,  // 使用可空的应用确认策略
          QString id = {});                                   // 构造稳定身份，未指定时自动生成
    QString id() const;                                       // 获取实例的稳定身份
    bool run(const AgentRequest& request,                     // 本轮输入历史、工具白名单和配置
             AgentResult& result,                             // 输出本轮增量及结束原因
             SdkError& error);                                // 同步运行；正常停止返回 true，异常失败返回 false
private:
    QString m_id;                         // 实例拥有的稳定身份
    std::unique_ptr<LLMClient> m_client;  // 独占的模型客户端
    ToolRegistry& m_registry;             // 应用拥有的注册表，不控制其修改时机
    ToolExecutor m_toolExecutor;          // 与 Agent 同生命周期的执行组件
    std::atomic_bool m_running{false};    // 阻止同实例并发或重入 run
};
}  // AiLib 命名空间结束
