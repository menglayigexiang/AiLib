#pragma once
#include <AiLib/Export.h>
#include <AiLib/tools/FunctionToolDefinition.h>
#include <AiLib/tools/ToolHandler.h>
#include <QHash>
#include <mutex>
#include <memory>
namespace AiLib {
// 工具执行器使用注册表中的共享条目。
class ToolExecutor;
// 应用拥有的工具集合；并发只读安全，涉及修改时由调用方同步。
class AILIB_EXPORT ToolRegistry {
public:
    bool registerTool(const FunctionToolDefinition& definition,  // 工具纯描述
                      ToolHandler handler,                       // 同步业务执行函数
                      SdkError& error);                          // 注册描述和业务函数，拒绝重名或非法 Schema
    bool removeTool(const QString& name);                        // 删除指定工具，修改时调用方负责同步
    bool contains(const QString& name) const;                    // 查询工具是否存在
    bool definition(const QString& name,                         // 注册工具名称
                    FunctionToolDefinition& output) const;       // 输出工具描述副本，未找到时不改输出
private:
    friend class ToolExecutor;
    // 保存业务函数及跨执行器共享的串行锁，描述模型本身不含运行时状态。
    struct Entry {
        FunctionToolDefinition definition;  // 工具的纯描述
        ToolHandler handler;                // 应用提供的执行函数
        std::timed_mutex executionMutex;    // 串行工具共用的执行锁
    };
    QHash<QString, std::shared_ptr<Entry>> m_entries;  // 注册表持有的运行时条目
};
}
