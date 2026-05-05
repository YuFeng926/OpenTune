# knowledge/ — Spec Extract 知识库

## 读取规则
- 日常读取：knowledge/current/（永远是最新激活版本）
- 历史回溯：knowledge/snapshots/（按日期版本归档）
- 运行时状态：.knowledge-work/（gitignore，不读）

## 目录职责

| 目录 | 职责 |
|------|------|
| current/ | 当前激活的完整知识库（模型读这里） |
| snapshots/ | 历史快照归档（二等公民，日常不读） |
| CURRENT.md | 当前版本元数据（版本号、时间、状态） |

## 入口

从 `current/_index.md` 开始，获取模块状态总览。
