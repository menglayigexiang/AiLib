# MCP 2026-07-28 互操作验证

## 范围

LibMcp 只验证 MCP `2026-07-28`，不运行旧协议兼容用例。互操作程序使用
Streamable HTTP，对 Tools 列表和调用执行真实线协议往返，而不是只比较本地类型。

## 已验证组合

| Client | Server | 结果 |
| --- | --- | --- |
| 官方 TypeScript SDK 2.0.0 | LibMcp | 通过 |
| LibMcp | 官方 TypeScript SDK 2.0.0 | 通过 |
| 官方 Python SDK v2.0.0 | LibMcp | 通过 |
| LibMcp | 官方 Python SDK v2.0.0 | 通过 |

四个方向均完成 `tools/list` 和 `tools/call`，并验证返回的 Tool 描述与结构化结果。
仓库内的 `libmcp_interop_server` 和 `libmcp_interop_client` 是可复用的 LibMcp
互操作端点；官方 SDK 驱动脚本使用对应 SDK 原生 Client/Server API。

Python 验证固定使用官方仓库 `v2.0.0` 标签。由于当时配置的包镜像未提供该版本及
完整构建依赖，验证环境从官方仓库标签安装；这不影响 LibMcp 的离线普通构建。

## 自动化边界

常规 CTest 覆盖官方 Schema、内存 Transport、真实 STDIO 子进程、Streamable HTTP、
SSE、分页、Schema 校验、MRTR、Progress、订阅和 TestApp 控件行为。测试不使用截图、
屏幕录制、图像识别或视频分析。
