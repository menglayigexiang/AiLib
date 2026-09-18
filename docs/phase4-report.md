# 阶段 4：Tool 子系统独立闭环

本阶段完成应用本地 Function Tool 的独立执行链，尚未接入 Agent Loop。核心同步执行，不创建工作线程，不依赖 GUI、信号槽或 Future。

## 文件和关键接口

新增公共头文件：

- `tools/ToolExecutionContext.h`：传递 `agentId`、`CancellationToken`、可选的 `steady_clock` 总截止时间。身份由未来 Agent 持有，Context 仅携带副本；截止时间为空表示不限时。
- `tools/ToolHandler.h`：既定 `std::function<bool(const ToolCall&, const ToolExecutionContext&, ToolResult&, SdkError&)>`。
- `tools/IToolApprovalProvider.h`：同步确认接口，输出 `Allow / Deny / Cancel` 和说明。
- `tools/ToolRegistry.h`：注册、删除、存在性查询、描述查询。注册拒绝空名称、空 Handler、重复名称、非法策略和不能完整校验的 Schema；失败不修改已有注册项。
- `tools/ToolExecutor.h`：非拥有的 Registry 引用和可空 Approval 指针，同步 `execute(call, context, result, error)`。

新增实现位于 `src/tools`：`ToolRegistry.cpp`、`ToolExecutor.cpp`、私有 `SchemaValidator.h/.cpp`。新增 `tests/unit/tst_Tools.cpp`，加入 Qt Test 和单头文件编译检查。`FunctionToolDefinition` 仍然只保存纯描述。

## 执行与结果语义

执行链为：检查核心调用/停止上下文 → 查找注册项 → Schema 参数验证 → 检查取消和截止时间 → 判断确认策略 → 应用同步确认 → 获取必要的串行锁 → 再次检查停止上下文 → Handler → 规范化结果 → 检查停止上下文。

`execute()` 的返回值：

| 情形 | bool | 输出 |
| --- | --- | --- |
| 业务成功 | true | 成功 ToolResult，SdkError 清空 |
| 未注册、参数错误、拒绝、确认不可用 | true | 失败 ToolResult，SdkError 清空 |
| Handler 返回 false 或异常 | true | 失败 ToolResult，SdkError 清空 |
| 共享 Token 取消、Approval Cancel | false | SdkError.category = Cancelled |
| 总截止时间到达 | false | SdkError.category = Timeout |
| 核心 ToolCall 的 id/name 为空 | false | 参数类 SdkError |

这里的 false 是交给后续 Agent 处理的停止/故障信号，不代表 Agent 必须返回失败。Agent 的主动取消与总超时仍需映射为既定的正常结束原因。没有增加 ErrorScope、Run 状态对象或每工具错误策略。

只有 true 的 ToolResult 适合正常反馈给模型；false 时不要将停止结果作为继续执行的工具消息。Handler 已执行后才发现取消或截止时间时，保留实际业务结果，不能撤销已经发生的副作用。

Executor 最终无条件重写 `callId / toolName`。成功结果清空错误字段；失败结果清空业务 data。Handler 返回 false 时使用其 SdkError 的 code/message，缺省补 ExecutionFailed 和说明；标准/非标准 C++ 异常都规范化为 ExecutionFailed。Handler 也可以返回 true 并通过 `result.success=false` 表达自定义业务失败。

## Approval 与并发

Never 不调用确认策略。Always 缺少 Provider 或确认机制失败/异常，产生 ApprovalUnavailable，绝不默认批准。Deny 产生 ApprovalDenied；Cancel 停止调用。

应用实现负责同步等待期间检查 Context 的 Token 和截止时间。SDK 能在确认前后检查，无法强行中断应用自己的阻塞函数。GUI 投递和等待由应用完成，CLI 输入同理，本阶段不实现 GUI 等待桥接。

Serialized 的执行锁保存在 Registry 内部工具条目中，由不同 Executor 共享；只有 Handler 阶段持锁，等待确认时不持锁。获取锁时每 20ms 检查协作取消和截止时间。Concurrent 不获取串行锁，应用负责 Handler 捕获对象的并发安全。

Registry 并发纯读取可用；任何修改与其他访问重叠时，应用负责同步。Registry 不被 Agent 锁定或冻结，没有 Run 快照、运行计数、修改检测。一次 execute 持有内部条目引用以保持 Handler 和锁生命周期，这不是本次 Run 的工具集合快照。应用需保证 Executor 使用的 Registry、ApprovalProvider 及 Handler 捕获对象仍然存活。

## JSON Schema 支持范围

本阶段实现显式受限的对象 Schema 子集，不宣称完整实现 JSON Schema：

- `type`：单一 object / array / string / number / integer / boolean / null。
- `properties`：递归对象 Schema。
- `required`：无重复的字符串数组。
- `additionalProperties`：仅 bool。
- `items`：单个对象 Schema，递归验证数组各项。
- `enum`：非空、无重复的 JSON 值数组。
- `title / description`：字符串注释字段。

空对象 Schema 不施加约束；建议工具根 Schema 声明 `type=object`。Schema 和实际验证路径递归限制 64 层。非法参数返回包含路径的 InvalidArguments（例如 `$.n[1]`），验证失败时不请求 Approval、不执行 Handler。

其他关键字（包括 `$ref`、`$schema`、组合 Schema、数值范围、正则、格式等）及 bool Schema 在当前注册契约中不支持，直接返回 InvalidToolSchema；不得用这些字段注册工具后假装已经完整校验。后续遇到具体工具需求再扩充校验子集或接入完整校验器。

类型、枚举及对象/数组适用规则核对了 [JSON Schema Validation](https://json-schema.org/draft/2020-12/json-schema-validation) 与 [JSON Schema Core](https://json-schema.org/draft/2020-12/json-schema-core)；当前限制是 SDK 的实现范围，不是标准自身的限制。

## 验证结果

- macOS、Qt 6.11.1、C++17 编译通过。
- CTest：9/9 组通过。
- 新增 Tool 测试：22 个实际用例通过（Qt Test 显示 24，包含 init/cleanup 两个钩子）。全项目累计 134 个实际用例。
- 覆盖注册/删除/查询、关联字段纠正、错误规范化、标准及未知异常、三种确认决定、确认故障/缺失、参数类型/必填/额外字段、嵌套数组和枚举、执行前后取消、总截止时间、共享串行等待取消、Concurrent 并行进入，以及等待确认时另一个执行器仍能执行同一工具。
- 37 个公共头文件分别独立编译通过。
- 安装到 `install/phase4`；仅引用安装头文件和动态库的独立程序完成注册 → 执行 → 结果 42 的闭环，退出码 0。
- Qt 5.15、最低 Qt 6.2、Windows/Linux 尚未实际验证。
- 本阶段离线验证，不调用真实 Provider，不增加 API Key 使用。

阶段 5 待验收后开始：将既有 Client 和 ToolExecutor 接入最小 Agent Loop。
