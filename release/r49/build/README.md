# R49 的唯一构建与获取入口

本目录绑定 `R49/source49-typed-tail-recovery-r1`，与主工作树保留的 source50 WIP 分开。不要运行旧 `contest_submission/build.sh`、旧同步清单或主树的通用构建命令，然后把结果称为 R49。`project-to-sdk.json` 是本交付的项目文件映射；主树、SDK和旧档案都不会被这些默认命令覆盖。

目前交付的是**有版本证据的源码整理包**。源码依赖闭合、整个固件从源码 clean build、冻结归档叠加方式的字节重现，是三个不同状态，以 `../dependencies/source-delivery-status.json` 为准。现已在新 RAM 私有目录重编译 R49 增量并链接；仅加入编译器的 debug prefix map 后，ELF 和 kernel 与原件逐字节相同。使用原始私有包装资源后，镜像也与固化 R49 相同。它仍使用已冻结父归档，不能写成“所有库已从源码重新构建”。

## 离线检查

从项目根运行，使用 Python 3.10+：

```text
python -B tools/export_r49_release.py
python -B release/r49/build/r49.py
```

第一条只检查本源树身份。第二条检查并列出缺少的私有文件、SDK及全源码构建状态；当前返回 **2 是预期的明确阻断**，不生成目标文件，不等于构建通过。哈希错误返回1。默认检查不读取本机凭据配置，不操作串口、服务、硬件、Git远端或SDK。

重新导出只能显式使用 `tools/export_r49_release.py --export`。它验证原1081项冻结pins和本地脱敏配置，使用真实冻结/历史源与已比对对象证据，不把 source50 当最终源。源内容不同的未知文件会被拒绝覆盖；它不删除旧资料。导出时脱敏配置不可用即停止，默认只读检查不依赖该配置。

## 源码各部分的用途

- `../source/project/`：R49最终增量，以及从真实父对象对应来源恢复的项目源。主文件优先保留R49最终实现。
- `../source/retained-variants/`：与最终实现不同的父版本源，用于对应父对象和比较，**不能与最终源一起通配编译**。
- `../source/unverified-snapshots/`：保留最初“原路径仍可读”的捕获位置。后续机器比较结果写入每文件及对象映射，不再搬动文件掩盖捕获历史；是否已证、比较范围和历史缺口逐项读取JSON，不能只凭目录名下结论。
- `../source/frozen-closures/`、`historical-closures/`：实际编译使用过的私有头闭包；“私有”在此也包括内部API头，不表示它们都含凭据。不同TU的历史include域保留分开，不把同名头随意合并。
- `../source/sdk-baseline-snapshot/`（如有）：从真实SDK捕获的板级/驱动与其他应用源。其原始SHA及与活动主树的差异单列，尚未逐一重编译到全部系统父库；不能用文件存在冒称对象等价。
- `../source/third-party-overlays/`：有原始来源证据的PFFFT C/H修复；上游原包和前后SHA在 `upstream.lock.json`。
- `../source/config/nuttx.config`：原R49的实际配置；SDK生成头、链接脚本和工具链另锁定。SDK快照中的旧CMake/Kconfig本身不证明已注册所有最终增量。

每份源的身份、验证等级在 `source-files.json`；对象对应关系在 `source-object-map.json`。只有实际原对象/重编译证据支持的条目才记对象等价。冻结文件身份验证与对象验证分别记录。

## 获取上游输入

查看固定SDK来源与提交，无网络操作：

```text
python -B release/r49/build/prepare_sdk_sources.py --plan
```

需要另建SDK上游源码副本时，显式 `--clone --destination <新私有目录>`；有现有SDK可加 `--from-sdk <原SDK根>` 从本地Git对象获取。只在新私有目录创建 detached checkouts，不覆盖原SDK，不把未提交改动静默带入。当前锁包含五个SDK仓及一个实际GCC工具链仓，另对gcc/g++/as/ld/ar/objcopy/objdump/nm/readelf逐个固定二进制SHA。实际SDK的26处修改、生成配置及缓存输入仍需分别核对；取得这些仓库不等于取得完整R49工作区。

APM 2.1和Abseil 20240722.1有原官方包、实际历史包SHA和PFFFT修复来源。可以离线使用现有合法原包：

```text
python -B release/r49/build/fetch_upstream.py --cache <新私有目录> --from-dir <原包所在目录> --extract
```

显式 `--download` 才从 `upstream.lock.json` 中固定的HTTPS官方URL获取。工具核对长度/SHA、安全解包，在新缓存中核对原PFFFT C/H再应用已锁定修复；不改官方原包或SDK。目录只能在项目 `private/` 或 `/dev/shm`，且不能位于原SDK。缓存创建失败时不自动删除旧数据。获取/解包成功只证明来源输入准备，不证明这些库的新原生编译已通过。

## 私密配置和资产接口

公开源树不携带真实业务凭据、微晶地址、模型本体、录音或生成音频数组。排除检查覆盖 C/H/INC 中的数组，不只看二进制后缀。`excluded-inputs.lock.json` 列出本次源导出排除的恢复接口、完整文件SHA和缺失原因；`audio-model-interface.json` 给出62个保留资产和10个新增本地提示的72个符号字节数/内容SHA，不含载荷。这不是固件所有视觉/KWS/ORT输入的穷举；其他模型来源与许可索引见 `../licenses/INDEX.json` 和 `../THIRD_PARTY_NOTICES.md`，62项缓存输入也不能替代它们的源重建。

`examples/` 给出：

1. 普通业务配置的三个 `static const char[]` 接口：credential、protocol version、proof；真实测肤业务另有第四个 consent reference。两种include属于各自编译域，不能合并成同一份定义，也不能伪造授权引用。
2. 微晶配置的三个宏接口：已核验的LE地址初始化、实际GATT值handle、操作者已核验标记。地址字节顺序必须依原栈和真实目标核对。
3. 私有配置字段schema及“接口路径→本机原文件”的精确恢复清单示例。

示例带 `#error`，没有可误用的输出目标、空PCM或伪造模型。示例值不能运行。**要重现原R49字节，操作者必须有权取得原精确私有输入**；自有新凭据、另一个设备地址或重新合成的声音会得到不同身份，应形成新的候选并另行验证，不能冒充原R49。

恢复原精确输入到新私有overlay：

```text
python -B release/r49/build/inject_private_inputs.py --input-map <私有JSON> --output <新私有overlay目录>
python -B release/r49/build/r49.py preflight --private-overlay <该overlay> --sdk <SDK根>
```

恢复工具先验证所有输入SHA；缺任一项就列出路径/原因并在创建输出前拒绝，不打印值，不生成替代数组。输出只能放项目 `private/` 或 `/dev/shm` 且不能在原SDK或release中。即便资产全部恢复，当前全源码clean闭合未完成时仍返回2。

## 已验证的冻结归档叠加回放

本模式给已有原冻结输入的操作者复现现有R49，明确依赖父归档，不是全源码构建入口。Linux SDK目前绑定原布局 `/home/swl/openvela`；工具不会创建系统别名或改SDK来适配别的目录。先读 `sdk.lock.json`、`toolchain-and-config.lock.json` 与62项 `frozen-link-inputs.lock.json`。未知SDK提交/已跟踪改动、工具、生成配置、源helper或缓存输入发生变化时拒绝。

在有合法原 `stage-r1.tgz` 的Linux环境执行：

```text
python3 -B release/r49/build/r49.py replay --sdk /home/swl/openvela --stage <原私有stage-r1.tgz> --output /dev/shm/<本轮唯一新私有目录>
```

输出目录必须新建且在 `/dev/shm`，不能在SDK或release内。本轮原回放环境的SDK盘与 `/tmp` 无空闲空间，因此所有临时编译文件也定向本轮RAM目录；这不是对接续者机器状态的推断。工具验证stage SHA、安全解包和1081项pins；只为GCC增加 `-fdebug-prefix-map=<本轮stage>=/dev/shm/velavision-microcrystal-r49-r1-20261103`，不改冻结源码、原pins或成品字节。

原始不同目录的回放只有kernel中GNU build-id的20字节不同；所有其他可分配节相同。prefix map后完整ELF、kernel及166个归档member与原件相同。归档容器元数据可不同，不能把容器SHA说成相同。公开身份摘要与20字节实际差分在本项目 `evidence/r49-source-delivery-20261104/source-binary-reproduction.json`；原始ELF/bin、完整节差分和构建日志保留在私有调查目录，不随公开包复制。不通过strip、归一化或补丁字节假装重现。

成功预期kernel：`2c41598a36403d4a41d6dfacd762b3ead83662d078e7d0679f031c10adb5bfc3`，33,397,864字节。回放输出只在私有目录，包含日志和原始ELF/bin；没有硬件加载、固化或远程提交。结束SSH前应由操作者保留需要的输出，本机最后一个登录会话结束后RAM目录可能被系统清理。

原包装资源、DTB和父镜像也没有复制到发布树。`packaging-inputs.lock.json` 只列其身份。可用 `package_private_image.py` 在私有目录、核对原父镜像及新kernel后包装；本轮已得到原镜像 `3479d1bd521ff143e44b6f2758558826bda6bf2f3148d1bfba21af7258c8689e`，33,548,288字节。

## 全源码clean构建的剩余边界

`r49.py clean` 当前明确拒绝。需要继续把系统/显示/无线/模型运行时及其他父库的每个保留输入对应到确切源、全部生成配置、原flags和依赖，再用新的源码编译结果替换62项缓存输入。不得把 `.a` 文件当作源码闭合，也不得把部分对象等价合成整个固件clean通过。剩余具体对象/来源见依赖锁；许可证与组合分发问题见 `../THIRD_PARTY_NOTICES.md` 与仓库根 `NOTICE`。当前根记录明确保留 source closure=false / clean build=false / frozen overlay byte reproduction=true。
