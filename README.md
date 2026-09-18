# AiLib

最小 Qt 项目：`LibAiCore` 为动态库，`TestApp` 链接该库并创建 QApplication 后退出。不包含窗口、库 API 或业务功能。

需要 CMake 3.21+、C++17 编译器，以及动态链接版本的 Qt 6.2+ 或 Qt 5.15。优先选择 Qt 6。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --config Release
cmake --install build --config Release --prefix "$PWD/install"
```

Windows 请将安装前缀替换为实际绝对路径。使用与 Qt SDK 匹配的编译器和架构。

根目录 `build` 保存统一的 CMake 缓存；`LibAiCore/build` 和 `TestApp/build` 保存各目标的中间文件。二进制分别位于 `LibAiCore/bin/<配置>` 和 `TestApp/bin/<配置>`，避免 Debug/Release 互相覆盖。macOS 程序产物为 `TestApp.app`。

根 CMakeLists 仅组织子项目与构建目录。各子项目自行配置版本、C++ 标准、Qt 模块、目标和安装规则；LibAiCore 选择 Qt 主版本，TestApp 使用相同主版本，避免混用 Qt 5 与 Qt 6。应用部署脚本位于 `TestApp/cmake`。

每个源码目录只有一个固定的中间文件目录，请勿同时从不同根构建目录配置项目；切换生成器、编译器或 Qt SDK 前删除三个 build 目录。

安装规则覆盖 Windows、macOS、Linux，需在目标平台本机分别构建。较新的 Qt 6 使用 Qt 官方 CMake 部署接口；Qt 5.15/Qt 6.2 在 Windows/macOS 调用对应 SDK 的部署工具，在 Linux 使用 CMake BundleUtilities 部署 Qt 库和平台插件。Linux 仍要求兼容的系统库及图形环境，不保证适用于任意发行版。

仅验证 Qt 5 时可添加 `-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON`。当前库源码为空；Windows 的占位导出源文件由 CMake 自动生成到 `LibAiCore/build`，保证可以生成 DLL 导入库。后续添加公共 API 时应使用显式导出宏。

本机已使用 macOS + Homebrew Qt 6.11.1 验证配置、编译，以及部署后 TestApp 的启动和退出。macdeployqt 仍报告可选 SVG 插件的 QtSvg framework 查找错误，因此当前验证不代表完整的插件部署通过；Windows、Linux 和 Qt 5.15 尚未实机验证。
