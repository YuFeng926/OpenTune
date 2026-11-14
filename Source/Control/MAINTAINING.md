<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# 维护控制接口 / Maintaining the control API

[中文](#维护控制接口--maintaining-the-control-api) | [English](#english)

> 读者：OpenTune 的维护者，以及帮他们改代码的 AI agent。
>
> 控制接口（本目录）让本机的其他程序——例如 OpenTune MCP Server——照界面的方式读写 OpenTune。OpenTune 的功能会不断变化，接口要跟着变。本文说明：改功能时接口这边要做什么，以及怎样通知接口的另一端（OpenTune MCP Server），让它及时适配。
>
> 规范正本在 [synchain-oss/opentune-mcp-server](https://github.com/synchain-oss/opentune-mcp-server) 的 `spec/otcp/v1/`；本仓库 `Tests/Control/spec/otcp/v1/` 是它的逐字节副本。

## 1. 什么时候要看这份说明

一次改动只要符合下面任意一条，就按本文处理：

- **新增**用户能用的操作：按钮、菜单项、快捷键、对话框、首选项、拖拽手势等，只要它会改工程、播放、视图或设置；
- **改变**已有操作的行为：参数、默认值、取值范围、撤销方式、作用于哪些轨道或片段、是否标记工程已修改；
- **删除或改名**一个操作、一个首选项或工程文件里的一个字段；

只改外观（主题、颜色、布局、字体、文案）不需要做任何事。

## 2. 三种做法，任选其一

| 做法 | 适合 | 你要做的 |
|---|---|---|
| **A. 只通知我们（默认，最省事）** | 新增或修改功能，你不想管接口 | 照第 4 节开一个 issue。我们来写规范、MCP 工具和这边的 C++ 代码，并给 OpenTune 提 PR 供你审 |
| **B. 顺手把接口也做了** | 改动小，你或你的 agent 有余力 | 照 `README.md`「Adding a method」实现；规范改动先照第 5 节提给 MCP 仓库（PR 或附上建议 schema 的 issue）；本仓库的规范副本不要直接改 |
| **C. 删除功能** | 某个功能退役 | 删除 `Source/Control` 里对应的方法、Ops 和 README 方法表条目，保持接口开关开启和关闭时的产品行为。删掉的方法所属能力不再上报，客户端收到 `UNSUPPORTED(capability_missing)`；同一能力内的方法不能单独退役，请先开 issue 商量。然后开 issue，让我们下线 MCP 那边的工具，并在规范里把这个能力标为退役（规范从 1.0.0 起只追加、不删方法） |

来不及处理时，可以先让受影响的方法返回 `UNSUPPORTED`，然后开 issue 说明；能力注册保持完整。

## 3. 实现边界

1. API 将请求转换为已有产品命令，并报告提交后的真实状态；不得改变 UI 的撤销、dirty、对象生命周期、ID 分配和渲染语义。`dryRun` 只做纯预测。
2. `OPENTUNE_ENABLE_CONTROL_API` 默认 OFF；接口关闭时，OpenTune 的行为与没有本目录时一致。
3. 内容只在消息线程上访问，并遵守 [README.md](README.md) 中的线程与生命周期约束。
4. 不要删除或挪走标有 `// OpenTune Control API hook` 的代码（清单在 `README.md`「Hooks」）。必须重写那段上游代码时，保留钩子并更新清单。
5. 不要直接改 `Tests/Control/spec/otcp/v1/` 下的规范副本；规范改动走第 5 节。

## 4. 给我们开 issue

- 仓库：<https://github.com/synchain-oss/opentune-mcp-server/issues>，模板「OpenTune change」。
- 一个改动一个 issue；开之前先搜一下有没有人报过：

  ```
  gh issue list -R synchain-oss/opentune-mcp-server --state all --search "<关键词>"
  ```

- 写清楚：
  1. OpenTune 的提交或 PR 链接，以及计划发布的版本（如果有）；
  2. 类型：新增 / 修改 / 删除 / 改名；
  3. 界面上的操作在哪里、怎么用，改动前后的行为（参数、默认值、撤销、是否标记修改）；
  4. 受影响的接口方法（查 `README.md` 的方法表；不确定就写「不确定」）；
  5. OpenTune 这边的接口是否已经改好（做法 B / C 的 PR 链接）；
  6. 你希望接口怎么表现（可选）。
- AI agent 可以直接用命令开 issue：

  ```
  gh issue create -R synchain-oss/opentune-mcp-server --title "<一句话>" --body-file <文件>
  ```

  正文按模板 OpenTune change 的小节写。
- 安全问题不要开公开 issue，请按该仓库的 `SECURITY.md` 私下报告。

## 5. 给我们提 PR

欢迎直接给 MCP 仓库提 PR（从 fork 提即可），例如补规范 schema 与向量、补一个工具参数。

- 先读该仓库的 `CONTRIBUTING.md`。要点：每个提交 `git commit -s`（DCO 签名）；提交信息与 PR 正文里不要带 AI 工具的署名行；提 PR 前跑 `npm run gates`。
- 改 `spec/**` 时同一个 PR 里附 `docs/contract-changes/<日期>-<主题>.md` 并更新 `spec/otcp/v1/CHANGELOG.md`。
- 该仓库是 MIT 许可，OpenTune 是 AGPL-3.0：**不要把 OpenTune 的源码、内部类名或内部结构写进 MCP 仓库**，只描述协议层面的行为。
- 规范合入并发布新的 spec 版本后，我们会给 OpenTune 提 PR 同步副本；你不用自己拷。

## 6. 我们这边会做什么

- 盯着该仓库的 issue（模板「OpenTune change」）与相关 PR，并尽快适配：更新规范、MCP 工具与文档，需要 C++ 改动时给 OpenTune 提 PR。
- 我们也会跟进 OpenTune 的功能变化，协调规范、MCP 工具和本目录的接口实现。
- 其他联系方式：<contact@synchain.ca>。

## 7. 给 AI agent 的规则（可原样粘进你的 CLAUDE.md 或 AGENTS.md）

1. 新增、修改、删除或改名一个会改工程、播放、视图或设置的界面操作时，按 `Source/Control/MAINTAINING.md` 处理控制接口：实现对应 API 行为，或给 `synchain-oss/opentune-mcp-server` 开一个「OpenTune change」issue。
2. API 复用已有产品命令，不改变 UI 的撤销、dirty、对象生命周期、ID 分配和渲染语义。
3. 不要删除或挪走 `// OpenTune Control API hook` 标记的代码；接口关闭时 OpenTune 的行为必须与原来一致。
4. 不要直接改 `Tests/Control/spec/otcp/v1/`；规范改动提到 MCP 仓库。
5. 开 issue 或 PR 前先搜重复；一个改动一个 issue；写清 OpenTune 提交链接与改动前后的行为。

---

# English

> For OpenTune's maintainers and the AI agents that help them.
>
> The control API (this directory) lets another program on the same machine, such as OpenTune MCP Server, read and edit OpenTune the way the UI does. OpenTune's features keep changing and the API has to follow. This page says what to do on the API side when a feature changes, and how to tell the other end (OpenTune MCP Server) so that it can adapt.
>
> The spec's source of truth is `spec/otcp/v1/` in [synchain-oss/opentune-mcp-server](https://github.com/synchain-oss/opentune-mcp-server); `Tests/Control/spec/otcp/v1/` here is a byte-for-byte copy.

## 1. When this applies

Any change that:

- **adds** an operation a user can perform (button, menu item, shortcut, dialog, preference, drag gesture) that changes the project, playback, the view or settings;
- **changes** what an existing operation does (parameters, defaults, ranges, undo, which tracks or clips it touches, whether it marks the project as changed);
- **removes or renames** an operation, a preference or a field of the project file;

Changes of appearance only (theme, colours, layout, fonts, wording) need nothing.

## 2. Pick one of three

| Option | When | What you do |
|---|---|---|
| **A. Just tell us (default, least work)** | A new or changed feature, and you would rather not touch the API | Open an issue as in section 4. We write the spec, the MCP tool and the C++ side here, and send OpenTune a pull request for you to review |
| **B. Do the API part as well** | The change is small and you or your agent have time | Follow "Adding a method" in `README.md`; propose the spec change to the MCP repository first (section 5: a PR, or an issue with the proposed schema); never edit the spec copy here directly |
| **C. Remove a feature** | A feature is retired | Remove its methods, Ops and method-table rows in `Source/Control`, keeping product behaviour unchanged with the API option ON and OFF. The capability of the removed methods is no longer reported and clients receive `UNSUPPORTED(capability_missing)`; methods within a capability cannot be retired individually, so please open an issue first. Then open an issue so that we take the tools on our side out of service and mark the capability as retired in the spec (from 1.0.0 on the spec only adds and never removes a method) |

If you have no time, make an affected method return `UNSUPPORTED` and open an issue; keep the capability registration complete.

## 3. Implementation boundary

1. The API maps requests to existing product commands and reports the committed state; it does not change the UI's undo, dirty state, object lifetime, ID allocation or rendering semantics. `dryRun` performs pure prediction only.
2. `OPENTUNE_ENABLE_CONTROL_API` is OFF by default; with the API off, OpenTune behaves exactly as without this directory.
3. Content is accessed only on the message thread, following the threading and lifecycle rules in [README.md](README.md).
4. Do not delete or move code marked `// OpenTune Control API hook` (list under "Hooks" in `README.md`). If that upstream code must be rewritten, keep the hook and update the list.
5. Do not edit the spec copy under `Tests/Control/spec/otcp/v1/`; spec changes go through section 5.

## 4. Opening an issue

- Repository: <https://github.com/synchain-oss/opentune-mcp-server/issues>, template "OpenTune change".
- One change per issue; search first:

  ```
  gh issue list -R synchain-oss/opentune-mcp-server --state all --search "<keywords>"
  ```

- Include: (1) the OpenTune commit or PR link and the planned release, if any; (2) the kind: added / changed / removed / renamed; (3) where the operation is in the UI and how it behaves before and after (parameters, defaults, undo, dirty flag); (4) the API methods affected (see the method table in `README.md`; "not sure" is fine); (5) whether the API here is already adapted (link to the option B or C pull request); (6) how you would like the API to behave (optional).
- An AI agent can open it from the command line:

  ```
  gh issue create -R synchain-oss/opentune-mcp-server --title "<one line>" --body-file <file>
  ```

  Write the body in the sections of the template "OpenTune change".
- Do not report security issues in public; follow that repository's `SECURITY.md`.

## 5. Pull requests to the MCP repository

Pull requests from a fork are welcome, for example a spec schema with vectors, or a new tool parameter.

- Read its `CONTRIBUTING.md` first. In short: sign off every commit with `git commit -s` (DCO); no AI-tool attribution lines in commit messages or PR text; run `npm run gates` before opening the PR.
- A change under `spec/**` comes with `docs/contract-changes/<date>-<topic>.md` and an entry in `spec/otcp/v1/CHANGELOG.md` in the same PR.
- That repository is MIT and OpenTune is AGPL-3.0: **do not put OpenTune source code, internal class names or internal structure into the MCP repository**; describe behaviour at the protocol level only.
- Once a spec change is merged and released as a new spec version, we send OpenTune a pull request that updates the copy; you do not need to copy it yourself.

## 6. What we do

- We watch the issues of that repository (template "OpenTune change") and related pull requests and adapt as soon as we can: spec, MCP tools and docs, plus a pull request to OpenTune when C++ changes are needed.
- We also follow changes to OpenTune's features and coordinate updates to the spec, MCP tools and this API.
- Other contact: <contact@synchain.ca>.

## 7. Rules for AI agents (paste into your CLAUDE.md or AGENTS.md)

1. When you add, change, remove or rename a UI operation that changes the project, playback, the view or settings, handle the control API as `Source/Control/MAINTAINING.md` says: implement the API behaviour or open an "OpenTune change" issue in `synchain-oss/opentune-mcp-server`.
2. The API maps requests to existing product commands and does not change the UI's undo, dirty state, object lifetime, ID allocation or rendering semantics.
3. Do not delete or move code marked `// OpenTune Control API hook`; with the control API off, OpenTune must behave as before.
4. Do not edit `Tests/Control/spec/otcp/v1/`; propose spec changes to the MCP repository.
5. Before opening an issue or PR, search for duplicates; one change per issue; include the OpenTune commit link and the behaviour before and after.
