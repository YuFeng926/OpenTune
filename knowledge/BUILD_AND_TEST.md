# Build & Test Knowledge - OpenTune

本文档记录 OpenTune 项目的编译和测试流程规范。所有自动化验证（无论是否需要用户手动操作）
都必须遵循本文档中的步骤。

## 核心规则

**每次执行编译和测试验证时，必须：**

1. 重新编译项目
2. 从项目根目录启动应用

**从项目根目录启动应用至关重要** -- 应用运行时会在当前工作目录的 `Logs/` 文件夹下生成
日志文件。如果不在项目根目录启动，日志将写入其他位置，无法在项目根目录下查看。

## macOS 编译流程

### 前置条件

- CMake 3.22+
- build 目录已通过 cmake configure 初始化（Makefile 方式）
- Git LFS 已拉取 `.onnx` 模型文件

### 编译命令

```bash
# 在项目根目录执行
cd build && cmake --build . --config Release 2>&1
```

### 验证标准

- 编译无错误（warnings 可接受，均为预存警告）
- 生成 `build/OpenTune_artefacts/Release/Standalone/OpenTune.app`

### 启动应用

**重要**：macOS 的 `open` 命令启动 `.app` 时，应用的工作目录默认是 `/`，**不是** shell
的当前目录。因此**必须直接执行二进制文件**，让应用继承 shell 的工作目录。

```bash
# 正确方式：直接执行 .app 内的二进制文件（后台运行）
# 必须在项目根目录执行，应用会继承当前 shell 的工作目录
# 这样 Logs/ 文件夹会生成在项目根目录下
./build/OpenTune_artefacts/Release/Standalone/OpenTune.app/Contents/MacOS/OpenTune &
```

**错误示范（不要这样做）：**

```bash
# 错误1：使用 open 命令，应用的 CWD 会变成 /，日志不在项目根目录
open build/OpenTune_artefacts/Release/Standalone/OpenTune.app

# 错误2：在其他目录下执行，日志不会写入项目根目录的 Logs/
cd build/OpenTune_artefacts/Release/Standalone && ./OpenTune.app/Contents/MacOS/OpenTune
```

## 测试验证体系

OpenTune 当前无自动化测试基础设施（CMakeLists.txt 中 CTest 已注释，无测试源文件）。
验证依赖编译检查和手动功能验证。

### 验证层级

| Level | Name | 说明 | Blocking |
|-------|------|------|----------|
| L0 | Spec Completeness | 验证 spec 文件结构完整性（Requirement/Scenario 数量、WHEN/THEN 格式） | No |
| L1 | Static Validation (Compile) | cmake 编译验证，确保无编译错误 | **Yes** |
| L2 | Unit Tests | 当前不可用（无测试框架） | No |
| L3 | Integration Tests | 当前不可用（无测试框架） | No |
| L4 | Contract Tests | 接口变更的代码 review 验证 | No |
| L5 | E2E Tests (Manual) | 用户手动操作验证，按 User Journey 逐项确认 | **Yes** |
| L6 | Regression Suite | 完整编译 + 应用启动 + 基本功能回归 | No |

### L1: 编译验证流程

```bash
# 1. 编译
cd build && cmake --build . --config Release 2>&1

# 2. 检查结果：无 error，生成 OpenTune.app
```

### L5: E2E 手动验证流程

1. **编译项目**（同 L1）
2. **从项目根目录启动应用**：`./build/OpenTune_artefacts/Release/Standalone/OpenTune.app/Contents/MacOS/OpenTune &`
3. **引导用户按 User Journey 逐项验证**，每个 Journey 包含：
   - 前置条件
   - 具体操作步骤
   - 验证标准（预期结果）
4. **逐项确认**：使用 AskUserQuestion 工具逐个 Journey 询问用户验证结果
5. **记录结果**：在 test-verification.md 中更新 Journey 状态

### L6: 回归验证流程

```bash
# 1. 完整编译
cd build && cmake --build . --config Release 2>&1

# 2. 从项目根目录启动应用（直接执行二进制，不用 open）
cd .. && ./build/OpenTune_artefacts/Release/Standalone/OpenTune.app/Contents/MacOS/OpenTune &

# 3. 确认应用正常启动、基本功能（音频导入、播放）可用
```

## 日志文件

应用从项目根目录启动后，日志文件位于：

```
<项目根目录>/Logs/
```

日志通过 `AppLogger::log()` / `AppLogger::debug()` / `AppLogger::error()` 写入，
可用于排查运行时问题。

## 完整的编译+测试+启动流程示例

```bash
# 在项目根目录执行

# Step 1: 编译
cmake --build build --config Release 2>&1

# Step 2: 直接执行二进制启动应用（后台运行，继承项目根目录作为 CWD）
./build/OpenTune_artefacts/Release/Standalone/OpenTune.app/Contents/MacOS/OpenTune &

# Step 3: 等待用户手动验证（如果有 L5 E2E 测试）
```

或者使用单行命令（推荐）：

```bash
# 在项目根目录执行，编译后直接启动
cmake --build build --config Release 2>&1 && ./build/OpenTune_artefacts/Release/Standalone/OpenTune.app/Contents/MacOS/OpenTune &
```

### 关于 `open` 命令的说明

macOS 的 `open xxx.app` 会通过 LaunchServices 启动应用，CWD 固定为 `/`。
而直接执行 `.app/Contents/MacOS/<binary>` 会让应用继承当前 shell 的工作目录。
AppLogger 使用 `juce::File::getCurrentWorkingDirectory()` 确定日志路径
(`Source/Utils/AppLogger.cpp:58`)，因此必须用直接执行方式确保 CWD 正确。
