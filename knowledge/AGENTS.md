# knowledge/ - OpenTune Knowledge Base

This directory contains reference documents that provide historical context,
design rationale, and implementation details for past changes. These are **not**
active specifications — they are supplementary reading material for agents and
developers working on related areas.

## When to Read These Documents

Read a document from this directory when:

- You are working on a feature that touches the same subsystem
- You need to understand **why** something was designed a certain way
- A code comment or `AGENTS.md` references a concept explained here
- You are investigating a bug in an area covered by one of these documents

Do **not** treat these as authoritative specifications. The source of truth is
always the code itself and the project-level `AGENTS.md` at the repository root.

## Documents

| File | Topic | Read When |
|------|-------|-----------|
| `MACOS_PORT.md` | macOS platform porting status, ORT integration, code signing, known issues | Working on macOS-specific code, build system, or platform compatibility |
| `unify-selection-mechanism-tasks.md` | Original task breakdown for unifying the piano roll selection model (HandDraw, LineAnchor, Note selection) | Working on piano roll selection, HandDraw/LineAnchor tools, or undo/redo in the pitch correction pipeline |
| `BUILD_AND_TEST.md` | macOS 编译流程、测试验证体系（L0-L6）、应用启动规范（必须从项目根目录启动以生成日志） | 执行编译验证、手动 E2E 测试、或编写 test-verification.md 时 |
