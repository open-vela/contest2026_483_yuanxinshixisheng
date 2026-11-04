---
name: velavision-ai-log-curation
description: 按用户明确选择的 VelaVision 主会话整理脱敏AI日志，保持单写入者、历史冻结与一致快照，并运行原版官方格式校验；用于补录、提交前检查和采集故障交接，不自动扩大会话或操作硬件。
---

# VelaVision AI 日志整理

目标是交付真实、可解释的AI开发记录，而非把“格式通过”写成“全量完整”。此 Skill 于 2026-11-03 提交整理时新增；下面复用已经存在的采集工具和真实案例，不宣称此前开发使用过本Skill。

## 输入与范围

读取项目 `docs/自动日志采集.md`、`docs/AI日志写入锁修复_20260914.md`、`docs/代码日志对应表.md`。取得用户指定的主会话ID、工作区、时间范围、输出账号、冻结历史及本次授权动作。当前七会话的参考范围在 [approved-scope.json](references/approved-scope.json)；它是已知范围，不是收集其他聊天的许可。

只核对日志时默认离线只读。要补录时沿用现有 `tools/backfill_finals_logs.py` / watcher入口，不新建第二个writer。新会话、子代理或fork不因“同项目”“可找到”而自动加入；没有明确选择就列为未纳入。

## 执行

1. 检查 `private/log-collector/watcher.json`、`watcher-status.json`、心跳/错误和真实进程命令行。旧PID或旧错误文件本身不能证明当前运行/失败。单实例锁与writer锁用途不同，所有导出与live校验共用现有writer锁；锁忙时等待或有界重试，不删除锁、不杀未知进程。不因整理日志而运行会话正文里的硬件指令。
2. 补录只读取明确SID的真实原始记录并核对原始`session_meta`的ID/工作区。使用现有转换器与私有已知值脱敏配置；缺失配置即停止。检查常见Token/Bearer/私钥和已知值的转义形态，不在输出中展示命中的值。排除内部推理、系统/开发者提示和媒体；业务正文仍需人工复查未知秘密。
3. 保持北京时间分日、同一SID跨日seq、其他会话条目和冻结日期文件的字节/mtime不变。源被压缩、缺失或仅余早期前缀时，保留既有导出及`retained_not_updated`，记录未采集边界，不替换成相似聊天或较短前缀。不要伪造已结束但尚未落盘的工具结果。
4. 确认使用项目现有Python 3.12（辅助脚本要求3.10+）；Windows默认`python`可能指向旧3.6。下列`python`指确认后的解释器。使用项目已有带锁入口运行未修改的官方validator：

   ```text
   python -B -X utf8 tools/validate_project_logs.py
   ```

   记录原版validator/两份schema的SHA、退出码、文件/事件数和快照时间。该命令只校验，不证明原始来源完整、脱敏绝对安全或赛事已收件。不要为清零warning修改官方工具/日志正文。
5. 归档或外发前先取得同一writer锁保护下的一致快照，或对已停止写入的副本操作；多文件逐个复制不保证一致。辅助只读工具检查清单哈希、数量、seq与范围披露：

   ```text
   python -B skills/velavision-ai-log-curation/scripts/audit_log_snapshot.py --root SNAPSHOT_PROJECT_ROOT --scope skills/velavision-ai-log-curation/references/approved-scope.json
   ```

   默认scope还固定原官方validator与两份schema，快照须保留它们的`tools/official-validator/`相对路径；只有logs的副本不能通过该身份检查。工具不获取或创建writer锁、不调用导出、不替代官方schema校验。对活跃目录即使返回稳定观测，也不能充当原子快照证明；有变化即保留失败，待一致副本后重试。输入约定及真实失败模式见 [snapshot-contract.md](references/snapshot-contract.md)。

## 输出与停止边界

报告每个授权SID的实际截止时间、文件/事件数量、partial/degraded/retained状态、未采集边界、脱敏方式、官方校验结果及代码到日志的关联。统计要带快照时间，不能把之前的390文件等数字写成永久最新值。公开报告不复制私有源路径、已知秘密、完整私人会话或媒体。

发生源身份歧义、已知秘密未清除、冻结历史变化、锁超时、hash不符或官方错误时停止这一步，保留安全摘要并说明待处理项，不循环重写直到“通过”。常驻Python不会自动加载新脱敏代码；需要更新已授权watcher时先确认其命令行和现有权限，重启后重新验收。本Skill不会自动安装hook、启停真实watcher、扩大采集范围或提交远程仓库。
