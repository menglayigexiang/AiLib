# LibMcp 与 TestApp 使用体验优化计划

## 1. 目标与边界

本计划从两类使用者出发：将 LibMcp 集成到 Qt/C++ 程序的开发者，以及使用
TestApp 配置、诊断和调用 MCP Server 的测试人员。

目标：

1. 第一次接入时能够沿一条清晰路径完成配置、验证、发现和调用。
2. 失败时能够区分配置、网络、HTTP、认证、协议版本、Schema 和业务错误。
3. TestApp 能覆盖 LibMcp 已实现的 Tools、Resources、Prompts 等主要能力。
4. 保持模块边界简单，不为未来场景增加 Factory、Provider 或插件体系。

固定边界：

- 只支持 MCP `2026-07-28`，不增加旧协议连接或自动降级。
- 不改变 Wire、Transport、HTTP facade 的既定依赖方向。
- 认证后续独立设计；本轮只为认证状态和错误保留清晰展示位置。
- 开发过程只分为“开发”和“测试＋返工修改优化＋测试”两个阶段。
- 验证禁止使用截图、录屏、实时视频分析和连续图像采样。

## 2. 主流 Agent 实现对比与取舍

本计划对照了 VS Code/Copilot、Cursor、Gemini CLI、Codex/ChatGPT 的公开 MCP
使用方式。对比关注使用体验和宿主责任，不采用它们的旧协议连接逻辑。

| 主流做法 | 其他 Agent 的处理 | 当前项目情况 | 本项目取舍 |
| --- | --- | --- | --- |
| Server 信任 | VS Code 在首次启动或配置变化后要求确认信任 | TestApp 可直接启动 STDIO 命令 | 增加首次启用确认和配置变化后重新确认；不在 LibMcp 协议层保存信任 |
| 工具权限 | Cursor、Gemini、Copilot 支持工具启用、禁用、允许和拒绝 | TestApp 可直接手动调用任意发现工具 | TestApp 根据 annotations 显示风险并对非只读/破坏性工具确认；LibMcp 只忠实暴露元数据 |
| 工具筛选 | Gemini 支持 includeTools/excludeTools，VS Code 可逐个开关工具 | Manager 缓存 Server 的完整工具列表 | 保留完整发现结果，宿主可按 Client 和工具名筛选；不修改线协议工具名 |
| 缓存更新 | VS Code 可清除缓存工具；多个 Agent 支持重启/刷新 | Manager 只在启用时加载一次工具 | 增加手动刷新、缓存时间和失效规则，并按 2026 订阅能力处理列表变化 |
| 状态与日志 | VS Code 可启动、停止、重启、查看输出 | 已有阶段状态，缺少统一通信记录 | 保留阶段状态并加入脱敏请求记录、重试和重新检测入口 |
| 配置作用域 | VS Code、Gemini 区分用户和工作区配置 | TestApp 当前只有应用级配置文件 | 增加清晰的应用级/项目级作用域，不让同一配置在界面中产生隐式覆盖 |
| 敏感输入 | VS Code 使用 input variables，Copilot 使用环境变量/Secrets | 已支持环境变量 Header，但仍允许固定 Header | 固定 Header 明示“可能包含敏感值”；导入和历史不复制明文密钥，优先环境变量引用 |
| 工具身份 | Gemini 为工具增加 Server 命名空间，避免多 Server 重名 | 当前通过所选 Client 隔离，不会误调另一个 Server | 内部始终使用 `(clientId, toolName)` 复合身份；发送请求时仍使用原始 toolName |
| OAuth | Codex/ChatGPT 使用 OAuth 2.1、PKCE 和资源元数据发现 | 尚未实现 OAuth | 继续作为独立安全设计，不下沉到通用 HTTP facade |
| 结果展示 | Cursor 将参数和结果做成可展开视图 | 已有 JSON 结果框和 Splitter | 增加摘要/原始 JSON 双视图、复制和耗时信息，不隐藏协议原文 |
| Resources/Prompts | VS Code 可浏览 Resources，部分 Agent 只消费 Tools | LibMcp 已支持，TestApp 尚未完整展示 | TestApp 补齐 Resources、Templates、Prompts，避免被 Agent 产品的 Tools-only 限制带偏 |

参考依据：

- [VS Code MCP Server 管理、信任、日志和缓存](https://code.visualstudio.com/docs/agent-customization/mcp-servers)
- [VS Code MCP 配置、敏感输入和命令](https://code.visualstudio.com/docs/agents/reference/mcp-configuration)
- [Cursor 工具开关、调用确认和展开结果](https://docs.cursor.com/context/model-context-protocol)
- [Gemini CLI MCP 配置、超时、信任和工具筛选](https://geminicli.com/docs/tools/mcp-server/)
- [GitHub Copilot MCP 工具允许与拒绝](https://docs.github.com/en/copilot/how-tos/copilot-cli/allowing-tools)
- [OpenAI MCP OAuth 与工具级安全声明](https://developers.openai.com/plugins/build/auth)

核心判断：LibMcp 是协议库，不应该替 Agent 宿主决定工具是否可以自动执行；它必须完整保留
Tool annotations、Server 身份和错误信息，让宿主能够安全决策。TestApp 是人工调试工具，应该
展示风险并在危险调用前确认，但不需要实现完整的 Agent 策略引擎。

## 3. 使用流程文字框架图

```text
┌──────────────────────────── TestApp 使用者 ────────────────────────────┐
│                                                                        │
│  Client 列表                                                          │
│  ├─ 添加 / 编辑配置                                                   │
│  │   ├─ STDIO：命令、参数、环境变量、工作目录                         │
│  │   └─ HTTP：URL、固定 Header、环境变量 Header、Bearer 环境变量      │
│  │                                                                    │
│  ├─ 测试配置                                                          │
│  │   └─ 配置检查 → Transport → HTTP → 2026 协议发现 → 能力摘要        │
│  │                                                                    │
│  ├─ 信任确认：首次启用或可执行配置变化后重新确认                       │
│  │                                                                    │
│  └─ 启用 Client                                                       │
│      └─ Starting → Discovering → Loading capabilities → Ready / Error │
│                                      │                                 │
│                                      ▼                                 │
│  Client 调试工作台                                                     │
│  ├─ 概览：Endpoint、ServerInfo、版本、能力、最近错误                   │
│  ├─ Tools：搜索、Schema、参数模板、调用、Progress、MRTR、结果          │
│  │          └─ 风险标记、调用确认、缓存刷新                           │
│  ├─ Resources：列表、模板、读取结果                                    │
│  ├─ Prompts：列表、参数、展开结果                                      │
│  └─ 请求记录：阶段、耗时、脱敏 Header、请求/响应、SSE 事件             │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
                                      │ 只调用公共 API
                                      ▼
┌──────────────────────────── LibMcp 使用者 ─────────────────────────────┐
│                                                                        │
│  McpClientManager                                                     │
│  ├─ 配置持久化                                                        │
│  ├─ enable/disable 语义                                                │
│  ├─ 状态、能力快照和结构化诊断                                        │
│  └─ Ready 后提供可直接使用的 McpClient                                │
│                                      │                                 │
│  McpClient                           ▼                                 │
│  ├─ discover / tools / resources / prompts / completion               │
│  ├─ McpOperation<T>：完成、取消、Progress、MRTR                       │
│  └─ 每个请求独立携带 2026-07-28 元数据                                 │
│                                      │                                 │
│  结构化诊断                          ▼                                 │
│  配置 → Transport → HTTP → Wire → 协议版本 → Schema → 业务处理         │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

## 4. 阶段一：开发

### 4.1 统一 Client 生命周期语义

修改位置：

- `LibMcp/include/LibMcp/McpClient.h`
- `LibMcp/include/LibMcp/McpClientManager.h`
- `LibMcp/src/client/McpClient.cpp`
- `LibMcp/src/client/McpClientManager.cpp`
- `docs/libmcp/README.md`

做法：

- 明确底层 `McpClient` 的启动只代表 Transport 可工作。
- Manager 对外使用“启用/禁用”概念表达完整流程，避免把网络连通称为 MCP 可用。
- 在公共 API 稳定前，评估将 Manager 的 `startClient/stopClient` 调整为
  `enableClient/disableClient`；如暂不改名，则通过注释和文档明确语义。
- Ready 的唯一条件是当前版本协议校验和声明能力的基础加载完成。

效果：使用者不会再把“HTTP 已建立”误认为“MCP 已经可以调用”。

### 4.2 结构化诊断与超时

修改位置：

- `LibMcp/include/LibMcp/McpResult.h`
- `LibMcp/include/LibMcp/McpOperation.h`
- `LibMcp/include/LibMcp/McpClientManager.h`
- `LibMcp/src/core/`
- `LibMcp/src/client/`
- `LibMcp/src/transport/`

做法：

- 为内部失败增加阶段信息：配置、Transport、HTTP、协议发现、能力加载、Schema、调用。
- 保留原始 HTTP 状态或 JSON-RPC 错误码，并给出简短可执行建议。
- 标记错误是否适合重试，不引入复杂异常层次。
- 为发现、列表和调用提供合理默认超时，并允许调用方按请求覆盖。
- 超时必须结束对应 Operation，不留下永久等待状态。

效果：调用方可以直接展示“在哪里失败、为什么失败、接下来怎么办”。

### 4.3 最短公共 API 示例

修改位置：

- `docs/libmcp/README.md`
- `docs/libmcp/examples/`（新增）
- 根目录或 LibMcp 示例构建配置

做法：

- 提供最小 HTTP Client 示例：启用、检查 Ready、读取工具、调用工具、处理错误。
- 提供最小 Server 示例：注册一个 Tool、启动、停止。
- 示例只使用公共 API，并纳入编译检查，避免文档代码失效。

效果：新使用者无需阅读内部实现即可完成第一次成功调用。

### 4.4 配置对话框增加“测试配置”

修改位置：

- `TestApp/LibMcp/McpClientConfigDialog.h`
- `TestApp/LibMcp/McpClientConfigDialog.cpp`
- 可新增 `TestApp/LibMcp/McpClientConfigurationProbe.h/.cpp`

做法：

- 在不保存配置的情况下创建临时 Client 并执行受控探测。
- 分步骤显示配置检查、Transport、HTTP、协议发现和能力摘要。
- 探测使用 LibMcp 公共 API，不读取 Wire 或 Transport 私有状态。
- 临时探测结束后保证 Operation 和 Transport 被取消、停止并释放。

效果：用户在保存前就能知道 URL、命令、环境变量和协议是否正确。

### 4.5 Client 状态详情

修改位置：

- `TestApp/LibMcp/LibMcpPage.h`
- `TestApp/LibMcp/LibMcpPage.cpp`
- 可新增 `TestApp/LibMcp/McpClientStatusPanel.h/.cpp`

做法：

- 列表只保留简洁状态：未启用、验证中、可用、协议不兼容、认证失败、网络错误。
- 选中 Client 后显示 Endpoint、ServerInfo、版本、能力、工具数量和最近错误。
- 错误详情不再全部塞入表格单元格。
- Error 状态保留启用意图，允许用户关闭后重新启用。

效果：列表易读，同时保留完整诊断信息。

### 4.6 补齐能力调试页面

修改位置：

- `TestApp/LibMcp/McpClientWorkbench.h`
- `TestApp/LibMcp/McpClientWorkbench.cpp`
- 按职责拆分新增 Tools、Resources、Prompts 子页面，避免工作台继续膨胀。

做法：

- 概览页显示发现结果和 Server 能力。
- Tools 页保留搜索、Schema、调用、Progress、取消和 MRTR。
- Resources 页支持资源列表、模板列表和内容读取。
- Prompts 页支持列表、参数编辑和展开结果。
- 根据 Server 声明的能力启用页面；未声明能力时给出明确说明。

效果：TestApp 可以验证 LibMcp 已实现的主要能力，而不只是 Tools。

### 4.7 JSON 参数编辑体验

修改位置：

- `TestApp/LibMcp/McpClientWorkbench.cpp` 或拆分后的 Tools 页面
- 可新增 `TestApp/LibMcp/JsonObjectEditor.h/.cpp`

做法：

- 从 `inputSchema` 生成可编辑的 JSON 示例，不开发复杂动态表单。
- 提供格式化、恢复示例、复制和清空按钮。
- JSON 语法错误显示位置；Schema 错误显示字段路径和原因。
- 每个 Client/Tool 只保存最近一次非敏感参数。

效果：用户不必从空白 `{}` 猜测字段，也能快速定位参数错误。

### 4.8 脱敏请求记录

修改位置：

- LibMcp 内部请求观测点，保持为精炼只读事件，不暴露 HTTP 底层类型。
- `TestApp/LibMcp/` 下新增请求记录模型和详情面板。

做法：

- 记录方法、阶段、耗时、HTTP 状态、JSON-RPC 错误和请求内事件。
- 展示经过脱敏的 Header、请求 JSON 和响应 JSON。
- 强制隐藏 Authorization、Bearer、Cookie、API Key 和环境变量密钥。
- 限制记录数量和单条文本大小，避免无限增长。

效果：开发者可以在 TestApp 内定位协议问题，不必额外抓包或修改日志级别。

### 4.9 保存工作台布局与非敏感历史

修改位置：

- `TestApp/LibMcp/McpClientWorkbench.cpp`
- TestApp 的 `QSettings` 使用位置

做法：

- 保存上下、左右分隔比例、当前标签、最近 Client 和最近 Tool。
- 保存最近非敏感调用参数；不保存 Token 或 Authorization Header。
- 配置不存在或布局版本变化时安全回退到默认值。

效果：用户调整好的阅读空间和调试位置在重启后保持不变。

### 4.10 Server 信任与工具风险提示

修改位置：

- `TestApp/LibMcp/McpClientConfigDialog.cpp`
- `TestApp/LibMcp/LibMcpPage.cpp`
- 拆分后的 Tools 调试页面
- TestApp 配置持久化模块

做法：

- STDIO 命令首次启用前明确提示该程序将以当前用户权限运行。
- URL、命令、参数、工作目录或环境变量引用变化后，使原信任决定失效。
- 工具列表展示 `readOnlyHint`、`destructiveHint`、`idempotentHint` 和
  `openWorldHint`，未知值按“未声明”处理，不能当作安全。
- TestApp 调用声明为破坏性或未声明只读属性的工具前进行确认，并展示实际参数。
- 信任和确认属于 TestApp 宿主行为，不加入 LibMcp Wire、Transport 或 Server。

效果：测试工具不会因为“只是调试”而静默运行本地程序或高风险远程操作。

### 4.11 工具缓存、刷新和列表变化

修改位置：

- `LibMcp/include/LibMcp/McpClientManager.h`
- `LibMcp/src/client/McpClientManager.cpp`
- `TestApp/LibMcp/McpClientWorkbench.cpp` 或拆分后的能力页面

做法：

- 工具快照记录获取时间，并提供显式刷新操作。
- Client 禁用、配置变化和重新启用时清除旧快照。
- 发现结果的 `ttlMs/cacheScope` 只用于明确的缓存决策，不推断隐式 Session。
- Server 支持 `subscriptions/listen` 时，根据列表变化事件刷新对应能力。
- 多 Server 工具始终以 `(clientId, toolName)` 标识；不得为了宿主命名空间修改
  发送到 Server 的原始名称。

效果：Server 更新工具后用户不必删除并重建配置，也不会误用另一个 Server 的同名工具。

### 4.12 配置作用域、安全导入与导出

修改位置：

- `LibMcp/include/LibMcp/McpClientManager.h`
- TestApp 配置持久化模块和 Client 列表页面

做法：

- 区分应用级配置与可选项目级配置，并在列表中显示来源。
- 同名或同 ID 冲突时明确提示，不做静默覆盖。
- 支持通过预览导入常见 `mcp.json` 传输配置；导入只转换 STDIO/HTTP 配置，
  不引入旧协议行为。
- 导出默认只包含环境变量引用和非敏感字段，检测到疑似密钥时阻止直接导出并提示。
- 固定 Header 可以保留，但界面明确提示明文保存风险。

效果：配置可以安全共享给团队，又不会把个人密钥或隐式覆盖一起带入仓库。

### 4.13 OAuth 作为后续独立设计项

本计划只记录边界，不在本轮直接实现：

- OAuth 不进入通用 HTTP facade。
- 授权模块负责取得和刷新 Token，Transport 只接收最终 Header。
- TestApp 后续提供登录、授权状态和退出授权。
- 在单独设计文档确认安全存储、回调和刷新流程后再开发。

## 5. 阶段二：测试＋返工修改优化＋测试

### 5.1 自动化测试

- 使用 Qt Test 查找控件、触发按钮、切换标签、调整 Splitter 并断言状态。
- 使用 FakeTransport 验证每一种诊断阶段和超时结果。
- 使用本地 HTTP Server 验证成功、HTTP 错误、错误 Content-Type、SSE、取消和超时。
- 使用本地旧协议特征 Server 验证只显示“不兼容”，绝不自动降级。
- 使用真实 STDIO 子进程验证命令、参数、环境变量和进程退出。
- 使用 InMemoryTransport 验证 Tools、Resources、Prompts、Progress 和 MRTR。
- 验证请求记录不会暴露 Authorization、Token、Cookie 或 API Key。
- 验证 STDIO 配置变化会使信任失效，未确认时不会启动进程。
- 验证破坏性或安全属性未知的工具在 TestApp 调用前出现参数确认。
- 验证工具缓存可手动刷新，配置变化和禁用会清除旧快照。
- 验证两个 Client 存在同名工具时不会交叉调用。
- 验证配置导出不会包含明文密钥，作用域冲突不会静默覆盖。
- 示例代码必须参与构建，避免 README 示例与公共 API 漂移。

### 5.2 使用流程验收

必须能够仅依靠界面完成：

1. 添加一个匿名 HTTP MCP Server。
2. 在保存前测试配置并看到协议版本和能力。
3. 启用后明确进入 Ready 或得到可执行错误建议。
4. 浏览并调用 Tool，查看完整参数、Progress 和结果。
5. 浏览 Resources 和 Prompts。
6. 从脱敏请求记录定位一次失败请求。
7. 重启 TestApp 后恢复布局和非敏感调试位置。

### 5.3 返工规则

- 测试发现职责混杂时先拆分模块，不向巨型类继续增加条件分支。
- 错误描述无法指导用户修复时，回到错误产生层补充结构化信息。
- UI 内容拥挤时优先调整信息层级、分页和 Splitter，不依赖扩大固定窗口尺寸。
- 公共 API 使用步骤仍然过多时，优先增加一个直接的便利入口，不建立抽象工厂。
- 每次修复先增加能复现问题的测试，再修改实现并重新运行相关测试和完整测试。

### 5.4 最终通过条件

- 完整 CMake 构建通过。
- 全部 Qt Test、LibMcp 协议测试和本地 HTTP/STDIO 测试通过。
- `git diff --check` 通过。
- 公共头文件不泄漏 jsoncons、Qt Network Reply、Socket 或 llhttp 类型。
- TestApp 不穿透 LibMcp 私有实现。
- 没有旧协议连接、初始化握手或版本降级代码。
- 没有截图、录屏、实时视频分析或图像识别测试。

## 6. 预期最终效果

```text
第一次使用：复制示例或填写配置 → 测试配置 → 启用 → Ready → 调用
发生失败：看到失败阶段 → 原始错误 → 修复建议 → 修改配置 → 重新测试
协议调试：选择能力 → 执行请求 → 查看进度/结果 → 查看脱敏通信记录
安全调用：确认 Server 来源 → 查看工具风险 → 审核参数 → 执行或取消
长期使用：布局、选项和非敏感参数自动恢复，密钥不进入配置和历史
```
