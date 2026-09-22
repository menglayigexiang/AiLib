# DeepSeek OpenAI Compatible 真实验证

使用现有 `OpenAIChatCompatibleAdapter`、Factory、LLMClient 和 QtHttpTransport，不新增 Provider 判断或 DeepSeek 专属 Adapter。

服务 API 根地址为 `https://api.deepseek.com`，模型为 `deepseek-flash`，使用调用方提供的 Key。请求使用非流式、maxOutputTokens=1024，以及 extraParameters 中的 `thinking.type=disabled`。地址及模型选择依据 [DeepSeek 官方首次调用文档](https://api-docs.deepseek.com/)。

## 真实结果

| 请求 | 结果 | 输入 / 输出 Token |
| --- | --- | --- |
| C++17 unique_ptr 普通问答 | 成功，有效文本 88 字符 | 21 / 42 |
| 指定 add(a=19,b=23) | 成功，生成 1 个完整 ToolCall | 311 / 47 |
| 人工执行加法并回传 sum=42 | 成功，最终文本包含 42，无后续调用 | 374 / 60 |

三次请求均通过 SDK 的完整成功及非 Length 检查。工具往返由手动测试程序驱动，不表示 Agent 或 ToolExecutor 已实现。本次没有改变 SDK 协议实现；验证范围为非流式 Chat 与 Function Calling，不涵盖 Streaming。

## 手动测试入口

新增 `tests/manual/deepseek_chat.cpp`。配置时启用 `AILIB_BUILD_MANUAL_TESTS=ON`，构建 `manual_deepseek_chat`，在调用方提供 `DEEPSEEK_API_KEY` 的进程环境运行。程序不保存凭据，不打印 HTTP 请求或原始服务端正文，只输出成功/故障及用量摘要，不注册到 CTest。

两个临时 Key 按用户要求保存于 `AGENTS.md` 顶部的醒目区块，使用 `TEMP_TEST_API_KEYS_BEGIN` / `TEMP_TEST_API_KEYS_END` 标记。后期删除整个“临时测试 API Key”区块即可；测试程序仍从运行环境读取凭据，不自动读取项目规则。

验证环境：macOS、Qt 6.11.1、C++17。手动测试目标编译及三次真实请求全部通过；CTest 7/7 组全部通过，git diff --check 通过。
