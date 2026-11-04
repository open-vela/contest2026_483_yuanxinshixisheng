# R46→R49：重复控制与插话收尾案例

本引用于 2026-11-03 为新 Skill 整理，历史修复并非通过调用本 Skill 完成。以下路径全部相对 VelaVision 项目根目录；先检查文件存在及收据身份。`private/` 是原始调查与冻结产物，不代表所有内容都应公开提交。

## 1. R45 后续失听，R46 隔离三种缺陷

入口：`docs/R46微晶重复语音控制修复_20261103.md`。

用户报告第二轮二三档之后停止无响应，重唤醒仍无效，真实报告到微晶阶段卡住；用户估计距唤醒已超过 40 秒。重新接入电脑后仅能确认语音退出、last_error/report_error=-5、BLE 6连6断/reason34、radio fault0。未采集的脱机历史无法恢复，reason34 的具体来源及首次漏停完整时序没有证明。

代码与测试区分了三项：底层 idle 门遗漏手动 active/uncertain；BLE 超时/迟到回调与退休状态污染后续会话；报告后的外设依赖失败退出整个语音 worker。它们是独立可复现缺陷，不等于每一项均被证明是首次实物漏停的直接原因。

| 证据 | 能支持的结论与限制 |
| --- | --- |
| `evidence/r46-microcrystal-repeat-20261103/source-freeze.json` | 冻结源码与实际候选的追溯入口，不能用 source50 WIP 替代 |
| `private/microcrystal-r46-20261103/idle-tests-r1/results/result.json` | 实际平台 idle 行为软件检查；不能单凭用户“40秒以上”倒推全部现场事件 |
| `private/microcrystal-r46-20261103/ble-tests-reconnect-r2/review.json` 与 `result.json` | 实际适配器 O0/O2、ASan/UBSan，旧回调、退休、四轮控制、超时和显式恢复；BLE 为 fake 边界，非无线长期实测 |
| `private/microcrystal-r46-20261103/report-tests-r6/summary.json` 与 `result.json` | 实际 runtime/service 的下一次监听、一次上传、不重播；专门外设错误 -2401 仅用于精确恢复，音频/取消/HOLD/时钟负控仍拒绝 |
| `evidence/r46-microcrystal-repeat-20261103/three-round-manual-acceptance.json` | 三轮 `011230/011230/0110` 及用户观察通过；第二轮最终启动1到二档60,943ms、到停机0为75,743ms；不是声学时延 |

该三轮收据明确 `physical_execution_ACK=false`。用户确认物理档位及停机，固件确认的是 GATT 回应。第一次“停止停止”未命中，后续裸“停止”成功；重复说法没有因此被标为支持。R46 此时仍 RAM 候选，原故障、功能与冷启动边界保留。

## 2. R47 手动建连提速没有解决新的语音退出

入口：`docs/R47微晶手动启动连接调优_20261103.md`。

用户确认慢在设备真正开始运行，已有连接下调档快。R47 只在精确手动 START 目标建连期间调整连接窗口，退出该阶段关闭租约；自动预连接、STOP 补连、手机配网和 0/1/300ms/1 序列保持。它不是删除先停机步骤或增加输出重试。

- `evidence/r47-microcrystal-faststart-20261103/manual-speed-acceptance.json`：三轮建连 2954/2074/1645ms，START 处理至完整启动 3552/2793/2595ms；用户反馈变快且调档停止正常。这是有限板端状态计时，非电机或外部声学测量。
- `evidence/r47-microcrystal-faststart-20261103/interruption-failure.json`：随后普通问答 ASR/TTS 成功、near cancel 已发生、echo 首错 -71、最终服务 -110，再无新 ASR 窗口。收据明确 `root_cause_proven=false`，记录故障而不是把 R47 写成最终通过。
- 原始日志路径由上项收据指向 `private/aliyun-candidates/r47-microcrystal-faststart-field-20261103-r1/live/console.raw`，哈希也在收据中。

## 3. R48：保留真实 EOF 顺序及取消语义

入口：`docs/R48语音插话收尾与晚取消修复_20261103.md`。

与现场元数据一致的原函数复现显示：自然 EOF_TAIL 后软件 cancel 改变校验模式，但底层仍按真实 EOF 顺序发 SOURCE_END，再在真实 RX 上到达 QUIET；硬取消模式要求另一顺序，产生 -71。实际 route/input 独立复现尾部阻塞导致 5000ms 取消票据退役超时 -110，排除了早先“两秒 rearm”猜测。现场没有精确记录被拒事件种类，不能称已经捕获那次实物完整分支。

R48 用严格 EOF cut/clear、零剩余/丢弃、完整前缀、同身份及真实 QUIET 证明原 EOF 尾部；保留 cancelled、终端 -125、旧 cancel0 和 PCM。旧包 ledger 的临时证明模式不得流到 model/sink。

- `evidence/r48-voice-tail-recovery-20261103/additional-software-regression.json`：全真实 source25 PIO/session/core 的自然 EOF、硬取消、完整 PF 交接及 next rearm O0/O2 证据；早期夹具失败保留，精确晚 ND/实物晚 cancel 不因此自动通过。
- `evidence/r48-voice-tail-recovery-20261103/speech-user-acceptance.json`：人工中途/句尾插话及后续问答通过，有6次 cancel/PF 交接和健康查询；`actual_APM21_EOF_CANCEL_branch_observed=false`。
- `evidence/r48-voice-tail-recovery-20261103/status.json`：人测通过后仍发现 old-cancel0 sink 风险；`final_persistence_allowed=false`。不能以用户“都正常”抹掉后续代码缺口。

## 4. R49：typed 事件只授权已证明的旧尾部 staging

入口：`docs/R49固化与重启录像_20261103.md`，冻结入口 `evidence/r49-finals-recovery-20261103/source-freeze.json`。

R48 旧 cancel0 ledger 虽能登记，普通 observer 仍走 route_clean；真实取消已有待交接尾部，route_clean 返回 EAGAIN 并留下 sticky failure。R49 的生产和消费两侧以私有 typed 事件传递窄能力，不改公开 packet/callback ABI。

| 侧别 | 保持的必要条件 |
| --- | --- |
| 生产端 | 完整原 EOF cut/clear/prefix/QUIET；OLD 包与 bridge 身份/上下文、真实连续性；完整320样本、索引与已接收上下界；原 PCM/cancel0 不变 |
| 消费端 | 已绑定 OLD 身份；实际 nd_onset 返回1后提交 cancel_next/fired；包在真实取消边界之后且正好连续；owner/input/mode/scope/lease/reply/TX/epoch 全部保持；report 为空 |
| 仍拒绝 | 普通 reason0 的旧 cancel0、未知/负 reason、错身份/代际、重放、部分帧、带 report、cancel1 假冒 typed、无真实取消边界或非法样本数 |
| 作用范围 | 仅选择原 experimental-tail staging；独立变换、真实 scope/TX 退休和 PF claim 许可继续原检查 |

直接证据：

- `private/microcrystal-r49-20261103/typed-cut-tests/review.json`、`result.json`：生产端原函数 O0/O2 与源码 pins。
- `private/microcrystal-r49-20261103/independent-handoff-tests/frozen-review-receipt.json`：7个实际 PIO/ND/route/PF 受控案例、每个优化14项消费端负控。observer 边界事件是显式输入；真实包 PCM/身份及 old cancel0 保留，不表示实板调度自动产生了罕见晚 EOF。
- `evidence/r49-finals-recovery-20261103/functional-acceptance.json`：三次 cancel/PF 交接、两轮手动成功、真实报告自动 `011320` 和恢复跟随；有一次建连超时，下一轮成功，ASR 有未命中。`physical_ACK` 仍为0；物理行为来自用户观察。
- `evidence/r49-finals-recovery-20261103/persistence-receipt.json`：单次写入和完整 SHA/逐字读回、GPT/相邻区核对；它自身早于冷启动，`cold_boot_verified=false` 是当时状态。
- `evidence/r49-finals-recovery-20261103/cold-boot-receipt.json`：后续真实断电启动与有限功能确认；OTG 拔除、调试 UART 保留，不是完全脱离电脑或长期验收。

R49 image SHA256 `3479d1bd521ff143e44b6f2758558826bda6bf2f3148d1bfba21af7258c8689e`，33,548,288字节；kernel SHA256 `2c41598a36403d4a41d6dfacd762b3ead83662d078e7d0679f031c10adb5bfc3`。正式重建必须从冻结证据出发，不能把主树 WIP 称为该镜像源码。

## 5. 复核本案例时的有限输出

R46 有跨 idle 门后指令有效的有限证据；R47 提速实测后又失败；R48 真人通过仍不足以关闭已知代码风险；R49 通过受控组件正负控与有限实机、读回、冷启动。每一层都有不同证明对象，不能把它们合成“全部错误已解决/每次连接成功”。

普通聊天插话与真实报告播放中打断是不同能力；本轮未把后者扩大为已验证。这里也不提供声学打断时延、临床效果、动态肤况护理方案或无线长期稳定结论。缺失某份原始资料时记为缺项，不以收据的文字反造原始日志。
