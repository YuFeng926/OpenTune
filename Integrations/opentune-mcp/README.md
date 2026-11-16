# opentune-mcp

OpenTune Standalone 的本地 MCP stdio 服务。提供 45 个工具：44 个工具对应现有 Control API 方法，`instances_list` 发现本机实例。编辑、预览、撤销、dirty 和音频处理均由 OpenTune 的共享产品命令执行。

## 安装与启动

环境：Node.js 24 LTS、启用了 Control API 的 OpenTune Standalone，以及 OpenTune 所需模型。客户端、MCP 服务与 OpenTune 使用同一台机器、同一用户。

在本目录安装锁定的依赖并构建：

```sh
npm ci
npm run build
node /absolute/path/opentune-mcp/dist/main.js --help
```

将 `node /absolute/path/opentune-mcp/dist/main.js` 配置为客户端的 MCP 命令。客户端启动 stdio 子进程，程序按需连接已运行的 OpenTune。stdout 承载 MCP 消息，诊断写入 stderr。

构建 OpenTune 时，在仓库根目录使用现有依赖与 CMake 配置，增加控制开关：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENTUNE_ENABLE_CONTROL_API=ON
cmake --build build --target OpenTuneStandalone
```

macOS 从应用内可执行文件启动，例如：

```sh
"/Applications/OpenTune.app/Contents/MacOS/OpenTune" --control-api
```

Windows 使用 `OpenTune.exe --control-api`。环境变量 `OPENTUNE_CONTROL_API=1` 同样启用运行入口。工程 I/O 和 UI 操作使用具有编辑器窗口的 Standalone。构建入口的默认值为 `OPENTUNE_ENABLE_CONTROL_API=OFF`，完整构建说明见仓库的 [BUILDING.md](../../BUILDING.md)。

## 发现目录

| 配置 | 目录 |
| --- | --- |
| MCP `--discovery-dir <absolute-directory>` | 显式指定目录 |
| `OPENTUNE_DATA_DIR` | `<OPENTUNE_DATA_DIR>/control/instances` |
| macOS 默认 | `~/Library/Application Support/OpenTune/control/instances` |
| Windows 默认 | `%LOCALAPPDATA%/OpenTune/control/instances` |

使用 OpenTune 的 `--control-api-dir=<absolute-directory>` 时，给 MCP 传入同一个 `--discovery-dir`。POSIX 发现文件保持当前用户私有权限。认证凭据从发现文件内部读取；客户端配置只包含程序路径和目录。

`instances_list` 返回公开元数据及损坏文件状态。`connectionState: "discovered"` 表示发现记录存在；业务工具首次调用时验证实例身份、serverProof、协议、capabilities 与 limits。应用未运行时仍可初始化 MCP、列出工具和获取空实例列表。

## 客户端配置

将示例中的 Node 和脚本路径替换为本机绝对路径。Windows JSON 路径中的反斜杠写作 `\\`。

- **OpenCode V2**：合入 [examples/opencode.json](examples/opencode.json)，使用 `mcp.servers.opentune`、`type: "local"` 和 `protocol: "auto"`。配置依据设计稿 2026-11-16 的 V2 契约。
- **Claude Code**：合入项目 `.mcp.json`，结构见 [examples/claude.mcp.json](examples/claude.mcp.json)。也可执行：

  ```sh
  claude mcp add --transport stdio --scope user opentune -- node /absolute/path/opentune-mcp/dist/main.js
  ```

- **Codex CLI**：合入 `~/.codex/config.toml`，结构见 [examples/codex.toml](examples/codex.toml)。示例将单次工具预算设为 60 秒，可容纳一次 30 秒任务等待。

工具权限沿用客户端自身的授权机制。SDK 2.4.0 的 stdio 入口同时支持经典 `initialize` 与 MCP 2026-07-28，自动化测试覆盖两条握手流程。

## 操作流程

1. 调用 `instances_list`，从结果选择 `instanceId`。后续每个业务工具都带该字段。
2. 调用 `session_info`、`project_get`，取得当前 capability、内容、placement、revision 和运行状态。
3. 使用读取到的 handles 与 revision 构造修改；支持 `dryRun` 的方法先预览，再使用相同的预期 revision 提交。
4. 从真实结果检查 `dirty`、`undo`、`warnings`、`sideEffects` 和任务状态。需要撤销时先读取 `edit_get_history`，再传入 `expectedUndoId`。

API handles、cursor、revision 和绝对秒数原样传递。`ContentSec`、`TimelineSec`、`OutputSec` 使用各自时钟，调用方直接采用 API 返回的位置。分页调用保留查询条件与 cursor；音符每页最多 500 项，曲线每页最多 10000 点。曲线建议从小范围与较小 limit 开始。

以下为 tool arguments；占位 ID 与 revision 从前序调用结果填入。

导入音频，工具 `import_start`：

```json
{
  "instanceId": "<instance>",
  "items": [{ "path": "/absolute/path/vocal.wav" }],
  "layout": "single",
  "newTrack": true,
  "startTimelineSec": 0,
  "generateNotes": true,
  "idempotencyKey": "vocal-import-001"
}
```

等待一次任务，工具 `job_get`：

```json
{ "instanceId": "<instance>", "jobId": "<returned jobId>", "waitMs": 30000 }
```

完成状态为 `succeeded`、`failed`、`cancelled`。工具成功启动任务时会立即返回 job 引用，任务成功取决于 `job.state`。一次 `job_get` 对应一次 OTCP 等待；返回仍在运行的快照时，由调用方决定下一次查询。取消 MCP 等待后，已发送的产品请求继续执行；`job_cancel` 请求取消产品任务。保存任务遵循 API 的不可取消语义。

预览音符修改，工具 `content_edit_notes`：

```json
{
  "instanceId": "<instance>",
  "contentId": "<content>",
  "expectedRevision": "<revision>",
  "ops": [{ "op": "setTargetPitch", "select": { "indices": [0, 2] }, "targetMidi": 60 }],
  "dryRun": true
}
```

提交时将 `dryRun` 改为 `false`。`REVISION_CONFLICT` 后重新读取状态并决定修改内容。工程重开后使用新 handles；应用重启后重新发现 instanceId。

导出，工具 `export_start`：

```json
{
  "instanceId": "<instance>",
  "kind": "master",
  "path": "/absolute/path/mix.wav",
  "ifExists": "error",
  "allowDry": false
}
```

检查导出任务终态后使用 `project_save`；新工程通过 `project_save_as` 指定 `.otproj` 路径。重开后渲染异步恢复，导出任务等待当前渲染完成。

## 返回与连接语义

成功结果同时写入 `structuredContent` 与 JSON 文本：

```json
{ "instanceId": "<instance>", "method": "project.get", "result": { "...": "original API fields" } }
```

OTCP 错误返回 `isError: true`，`error.source` 为 `otcp`，保留原始 `code`、`message`、`data`，包括 `executed`、`retryable`。适配器错误的 source 为 `adapter`，并包含执行状态：

| executionState | 含义与后续操作 |
| --- | --- |
| `not_sent` | 业务请求尚未发送，可依据错误修正参数、实例或连接条件 |
| `unknown` | 请求已发送，回复缺失；先读回工程、任务或文件状态，再决定下一步 |

同一 MCP 进程按实例复用一个认证连接，默认保留一个在途位置给 `job_cancel`。实例默认支持 4 个认证会话，第五个客户端获得原始 `BUSY(session_limit)`。连接断开后，下一次新调用重新发现并认证；已发送请求的执行状态保持 `unknown`。

stdin EOF、transport 关闭及 SIGINT/SIGTERM 释放 MCP 连接。退出 OpenTune 使用显式工具 `app_quit`。首次使用应用时，关闭 onboarding 会触发现有示例工程导览，`ui_get_state` 显示其 busy 状态。

## 验证与打包

在仓库中的本目录执行：

```sh
npm ci
npm run build
npm test
npm pack
```

`npm pack` 生成 `opentune-mcp-0.1.0.tgz`，可用 `npm install --global /absolute/path/opentune-mcp-0.1.0.tgz` 安装 `opentune-mcp` 命令。源码仓库安装后，保留 `dist` 与 `node_modules` 即可直接运行；运行链路使用本地文件和回环 TCP。CLI 的模型连接按该客户端的模型配置工作。

真实集成测试通过 `OPENTUNE_TEST_APP` 指定启用 Control API 的可执行文件：

```sh
OPENTUNE_TEST_APP=/absolute/path/OpenTune.app/Contents/MacOS/OpenTune npm run test:integration
```

Windows PowerShell：

```powershell
$env:OPENTUNE_TEST_APP = 'C:\OpenTune\OpenTune.exe'
npm run test:integration
```

测试启动专用实例，使用独立的 `OPENTUNE_DATA_DIR`、已完成导览的测试偏好及仓库音频素材；`OPENTUNE_TEST_AUDIO` 可指定一段可分析的人声音频。产物目录由测试输出，保留工程、WAV 和日志用于检查。常规 `npm test` 在缺少 `OPENTUNE_TEST_APP` 时跳过真实实例测试。

已完成 macOS x64、Node 24.21.0、SDK 2.4.0 和 Control API 启用的 OpenTune 1.8.2 验收：

- 协议测试：分包/粘包、UTF-8、乱序响应、字节上限、身份认证、权限、连接复用、取消、容量、断线及 EOF。
- 契约测试：45 项工具目录、全部 44 个方法/capability 映射、有效输入与边界输入、经典及新 MCP 握手。
- 真实实例：45 项工具全部调用；预览、修改、revision 冲突、撤销/重做、编曲、网格、设备、任务取消、会话容量、保存重开与退出。重开前后导出的 WAV 字节一致。
- 离线控制链路：`npm ci --offline` 重装成功；macOS 沙箱仅允许 localhost 网络连接时，17 项测试全部通过，包括真实实例闭环。

Windows 实机验收、三个 CLI 的具体发行版联调及 GUI 手动修改后的冲突场景列为后续平台验证项；当前自动化客户端使用官方 SDK 的经典和新协议模式。

源代码：[OpenTune / Integrations/opentune-mcp](https://github.com/YuFeng926/OpenTune/tree/main/Integrations/opentune-mcp)。许可：AGPL-3.0-or-later。
