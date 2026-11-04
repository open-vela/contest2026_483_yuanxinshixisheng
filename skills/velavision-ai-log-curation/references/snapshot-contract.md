# 快照检查与真实失败模式

辅助脚本输入为项目根副本，以及显式scope。使用随附`approved-scope.json`时，副本必须保留`logs/<member>/manifest.json`、JSONL和`tools/official-validator/tools/validate-log.py`、`tools/official-validator/schema/{event,manifest}.schema.json`的原相对路径；仅含logs的副本会因工具pin缺失失败。原工具和schema来自已核对的项目文件，不为通过检查删除期望pins。取得快照时先在现有writer锁保护内复制日志清单与分区，再核对复制前后哈希；不要修改运行中的writer。

scope要求`schema_version=1`、`member`、`team_id`、`tool`、非空唯一`session_ids`；可配置`require_partial_disclosure`及官方validator文件pins。它只检查所选会话，不删除或认定其他manifest会话越权。

每个所选会话需有`files`数组，数组项为`file_path`/`sha256`/`event_count`，路径只能位于该member下；同时检查顶层累计数量/会话流SHA、身份和严格递增seq。partial/degraded/error须有非空缺失说明；本项目参考scope要求继续保留partial/degraded披露。检查结束重新核对manifest内容与所有已读文件的身份/大小/mtime，检测到变化视为失败。检查没有获得进程间锁，因此“本次读数未变化”不是原子副本或全来源完整的证明。

输出不含事件正文、秘密或机器绝对路径；`source_completeness_proven=false`、`official_schema_validation_run=false`始终保留。退出码0为所列交叉检查通过，1为证据/披露/快照不符，2为输入不合法。原版官方校验须另行运行，不可把本工具名当作官方工具。

真实记录中的处理方式：

- 2026-09-14 Windows字节锁竞争原先在加锁前读锁字节时报PermissionError；已由现有项目writer修复。不能删锁绕过或新开不持锁的导出入口。
- JSONL分日文件先替换、manifest后替换，独立直接运行validator可能读到混合快照。使用`tools/validate_project_logs.py`带同一writer锁包装原版工具。
- 常驻watcher曾未加载后续Token脱敏规则。修改源文件不代表驻留进程已更新；需在授权范围内核对命令行后重新加载，且不能打印敏感匹配值。
- Codex源可能只剩压缩早期前缀。现有backfill保留旧交付和`retained_not_updated`，不以短前缀替换较新记录；这一状态与官方格式通过可同时成立。
- 仅采集七个显式主会话和北京时间2026-10-10起的新范围，早期已交付记录冻结。seq连续仅说明导出排序，不证明中间没有省略；内部推理/提示、媒体、子代理仍未导出。

这些是既有工具的历史案例，此Skill是2026-11-03新增整理，不倒填使用历史。活跃状态/范围以最新真实收据与用户指令为准。
