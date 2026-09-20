# VelaVision —— 基于 openvela 的 AI 皮肤检测与云台护理视觉机器人

> 队伍：远信实习生（`contest2026_483_yuanxinshixisheng`）｜硬件：KICKPI K7（RK3576，4+32GB，实板无线 VS6621SR80）
> 成员 / AI 日志目录：`allenxun`、`maomaojiang01`、`liyoface`、`lzttt1111`
> 交付索引见 [`contest_submission/`](contest_submission/README.md)；逐版本构建与真机证据见 [`evidence/`](evidence/)；需求、实现、测试与**失败**记录见 [`docs/`](docs/)。

## 一、作品简介

VelaVision 是一套跑在 **KICKPI K7（RK3576）原生 openvela/NuttX** 上的视觉机器人作品，把"新硬件适配"做成了一个能用的 AI 硬件产品：

- **板端**：摄像头取流与 JPEG、人脸/姿态检测与跟随、云台运动协议、BLE 加密配网 + 原生 WPA2/DHCP 联网、语音阶段提示、照片上传与任务状态；
- **云端**：AI 皮肤检测评分（V2 十二项/四光源、水光三项云服务、PC 目标检测与拍照）、人脸比对、护理计划与报告讲解、异步任务编排；
- **手机端**：Android（AISIA）负责配网、测肤拍摄与 Filament 3D 展示；另有 Node/mjs 联调客户端用于蓝牙配网与语音链路验证。

完整链路为：**配网 → 拍摄 → AI 评分 → 护理方案 → 云台执行 → 报告播报**。

亮点：

1. **原生 BSP 与驱动适配**：RK3576/K7 板级初始化、UART、USB Host（xHCI）、BLE 与 Wi-Fi 共存、NPU 诊断路径、DDR/eMMC 与文件系统边界，均以 NuttX 原生方式实现并留证，不靠 Linux 参考代码代替验收。
2. **诚实的验收口径**：编译通过 / RAM 加载 / 真机验证分开表述；未通过项在 [`contest_submission/submission-checklist.md`](contest_submission/submission-checklist.md) 里明确列为"不得写成已通过的项目"。
3. **契约先行的云端后端**：OpenAPI 契约为唯一出处（`backend/contracts/`），Java 21 + Spring Boot 3.5.16 + Flyway + PostgreSQL 16 与 Python 3.12 异步 Worker（at-least-once + 领取代次 `lease_revision` 防重复落地），并配 **94 场景黑盒验收框架**（`backend/acceptance/`）与可复跑证据。
4. **AI 开发全流程留痕**：`logs/` 下 4 位成员共 **436 个会话、297,761 条事件**（截至 2026-09-20 本说明提交时），全部通过组委会官方 `validate-log.py` 校验（各目录 `manifest.json` 为数量、文件与哈希入口）。

## 二、选题方向

**新硬件适配**（主线）＋ **AI 硬件产品创新**（作品形态）。

理由：官方硬件清单把 KICKPI-K7 明列为待适配平台，`vendor_rockchip` 的 K7 页面也标记尚未适配，因此本作品以 RK3576/K7 的**原生 openvela BSP、无线与视觉驱动适配**为基础交付；在此之上叠加 AI 皮肤检测与云台护理的产品闭环，让适配工作有真实业务负载可验证，而不是只跑通串口。

- 赛道逐条对齐与缺口：[`docs/官方硬件赛道对齐与目标_20260909.md`](docs/官方硬件赛道对齐与目标_20260909.md)
- 官方要求 ↔ 本仓入口对照表：[`contest_submission/submission-checklist.md`](contest_submission/submission-checklist.md)
- 源码分级（A 参赛主线 / B 配套实现 / C 文档证据 / D 本地归档）：[`contest_submission/source-map.md`](contest_submission/source-map.md)

## 三、目录结构

**板端（openvela/NuttX，RK3576 K7）**

- `board/kickpi_k7/` — K7 板级：多套 defconfig（含 `velavision_wifi_ip_local`、`velavision_cloud_speech_local`）、板级初始化、内存/GIC/UART 说明
- `port/` — RK3576 芯片级与公共层适配；`port/tracked/` 保留对 NuttX 等公共仓的改动
- `patches/`、`config/` — 公共层补丁、构建与运行配置模板（如 `agent-model.example.json`）
- `app/` — 22 个板端应用模块：`k7host`（摄像头/JPEG/YuNet/跟随）、`k7npu`（NPU 诊断）、`k7radio`（BLE+Wi-Fi）、`k7usb`（USB Host）、`gimbal`（云台协议）、`k7agent`（语音意图/任务/照片上传）、`k7audio`·`k7sound`·`k7audiohw`（音频）、`k7graph`、`k7mem`·`k7emmc`·`k7storage`·`k7fat`（内存与存储边界）、`k7eh`·`k7ehcontrol`、`k7cxx`·`k7neon`·`k7smp`·`k7load`·`k7arena`（运行时与诊断）、`voicelink`（语音链路候选）
- `mcu/` — STM32 云台/外设控制器配套（`stm32-v1.3`）

**云端 / 后端**

- `backend/web-java/` — Spring Boot 3.5.16 + Java 21 业务后端；Flyway 为唯一迁移入口
- `backend/worker-python/` — Python 3.12 异步任务 Worker（claim/dispatch/complete + 租约回收 + 健康端点）
- `backend/face-service/` — InsightFace 人脸服务（**明确声明不支持活体检测**，`require_liveness=true` 返回 501）
- `backend/llm-rag-application/` — AI 服务模块：FastAPI + Streamlit 的 RAG（bge-m3 向量 + bge-reranker-large 重排 + pgvector），含医疗云 AI 集成契约
- `backend/contracts/` — OpenAPI 契约与跨语言规范化说明（唯一契约出处）
- `backend/acceptance/` — 94 场景黑盒验收框架与运行证据
- `backend/deploy/` — 构建、启动与测试脚本（含隔离 PostgreSQL 规则、Docker Compose）
- `backend/doc/` — 设计文档与文档站（`site/` 源码 + `web/` 构建产物）
- `backend/handoffs/`、`backend/tests/` — A–E 工作包交接与 Oracle 评审记录、跨模块端到端验收

**AI 算法、主机联调与 Skill**

- `host/skin_algorithms/` — 皮肤算法（V2 十二项/四光源、水光三项、PC 目标检测与拍照），说明见 [`contest_submission/skin-algorithms.md`](contest_submission/skin-algorithms.md)
- `host/vision/`、`host/voice/`、`host/streaming_asr/`、`host/cloud_speech_bridge/`、`host/whole_device/`、`host/support/` — 主机侧联调网关与验收脚本
- `skills/` — 自建 Skill：`k7-openvela-driver-port`（驱动移植与证据流程）、`skin-vision-toolkit`（视觉）

**前端**

- `fronted/` — AISIA Android 工程（CameraX 测肤、K7 BLE 配网、Filament 3D、OpenVela 鉴权）
- `frontend/` — Node/mjs 联调客户端（蓝牙配网、语音、报告流对接说明与示例）

**复现、测试、证据与文档**

- `tools/` — `build.sh`、`sync_sdk.py`、`export_project_logs.py`、`official-validator/` 等
- `tests/` — O0/O2 回归与板端测试
- `evidence/` — 构建哈希、串口摘要、稳定性与验收证据（按版本分目录）
- `docs/` — 需求、实现、测试、失败与交接记录（**不删除失败记录**）
- `deliveries/` — 作品介绍文档（docx/pdf）、演示视频与提交包
- `contest_submission/` — 比赛交付索引：`README.md`、`source-map.md`、`submission-checklist.md`、`submission-manifest.json`、`skin-algorithms.md`

**AI Coding 材料与归档**

- `logs/` — AI Coding 日志：`allenxun/`（402 会话 / 101,741 事件）、`lzttt1111/`（5 / 146,970）、`maomaojiang01/`（6 / 48,043）、`liyoface/`（23 / 1,007）
- `legacy/`、`baselines/`、`vm-archive/`、`work-in-progress/`、`third_party/`、`deploy/` — 历史归档、未完成候选与外部依赖，**不作为当前产品入口**
- 根目录：`contest2026_483_yuanxinshixisheng.xml` 与 `openvela.xml`（repo manifest，含 `<linkfile>` 映射）、`project-manifest.json`（SDK 基线、构建命令、产物哈希、必需 `CONFIG_*`）、`AGENTS.md`（本仓 AI 协作规则）

## 四、运行方式

### 0. 前置

- Ubuntu SDK 工作区（文档示例路径 `/home/swl/openvela`）。基线：nuttx `e987a81c32cab008d1a8521669e5488d00271322`、apps `92cc8c56a4c9b780b38991401295179fa2461908`（见 `project-manifest.json`）
- 硬件：KICKPI K7（RK3576 4+32GB）、USB-OTG 线、串口线；一部 Android 手机装 `fronted/` 构建出的 App 用于 BLE 配网
- 云端：JDK 21 + Maven、Python 3.12（Worker）与 3.11（AI 服务）、Docker（PostgreSQL 16）

### 1. 板端固件（openvela/NuttX）

```bash
python3 tools/sync_sdk.py --sdk /home/swl/openvela --check   # 只读核对 SDK 差异；遇未知改动会停止，不静默覆盖
bash tools/build.sh /home/swl/openvela                       # 构建，产物 nuttx / nuttx.bin
```

- manifest 的 `<linkfile>` 已把 `app/k7host → apps/examples/k7host`、`app/gimbal → apps/examples/gimbal`、`app/k7npu`、`app/k7radio`、`app/k7usb`、`board/kickpi_k7 → nuttx/boards/arm64/rk3576/kickpi_k7` 映射进 SDK；**不要把源码复制到第二套目录**，否则 linkfile、构建脚本与日志中的路径会失效。
- 必需配置（`CONFIG_EXAMPLES_K7HOST*`、`CONFIG_EXAMPLES_GIMBAL`、`CONFIG_RK3576_NPU_DIAG`、`CONFIG_WIRELESS_BLUETOOTH_HOST`、`CONFIG_RK3576_USBHOST` 等）与产物 SHA256 记录在 `project-manifest.json`；每次构建证据写入 `evidence/build/<版本>/verification.json`。
- 上板：当前主线是 **OTG RAM 启动**（未刷 eMMC）；入口为串口 NSH，无线/视觉/NPU 命令都需显式调用（RAW NPU 仅为隔离诊断实验）。

### 2. 云端后端（`backend/`）

```bash
cd backend/deploy/dev
./pg-up.sh                     # 幂等启动隔离 PostgreSQL 16 容器（只绑 127.0.0.1:55432，不碰宿主 5432）
./createdb.sh mvp_a_dev        # 建库（幂等）
./migrate.sh mvp_a_dev         # Flyway 迁移（唯一迁移入口；Python 侧永不迁移）
./run-java-tests.sh            # Java 全量测试（含迁移集成测试，自建临时库）
./run-python-tests.sh          # Python venv pytest + python -m mvp_worker --check

cd ../../web-java
mvn -q -DskipTests package
SPRING_DATASOURCE_URL=jdbc:postgresql://127.0.0.1:55432/mvp_a_dev \
SPRING_DATASOURCE_USERNAME=postgres SPRING_DATASOURCE_PASSWORD=mvp_a_local \
SERVER_PORT=18080 java -jar target/web-java-0.0.1-SNAPSHOT.jar
curl --noproxy '*' http://127.0.0.1:18080/actuator/health          # {"status":"UP",…}

cd ../worker-python
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'
.venv/bin/python -m mvp_worker            # 运行循环；--once 单周期，--recover 回收过期租约
                                          # 健康端点默认 127.0.0.1:8081 的 /healthz /readyz

bash backend/tests/run-acceptance.sh      # A 包跨语言端到端验收
```

> `mvp_a_local` 是本机开发占位口令，非真实凭据。整套也可用 `cd backend/deploy && cp .env.sample .env` 后走 Docker Compose（pg + web + worker + nginx）。

### 3. AI 服务（`backend/llm-rag-application/`）

```bash
cd backend/llm-rag-application
uv sync                                       # 依赖由 uv.lock 锁定；Python 3.11（要求 >=3.10,<3.12）
export APP_LLM_API_KEY=<你的密钥>              # 密钥只走环境变量，仓库内为空
python start.py                               # 同时拉起 FastAPI(默认 7861) 与 Streamlit(默认 9003)
```

- 需要 PostgreSQL + pgvector；所有配置项见 `conf/config.yaml`，每项都有对应的 `APP_*` 环境变量。
- 首次运行会从 ModelScope 拉取 `BAAI/bge-m3` 与 `BAAI/bge-reranker-large` 到 `./models/`（仓库不含模型权重）。
- 人脸服务部署见 `backend/face-service/deploy/README-deploy.md`（InsightFace `buffalo_l`，**无活体检测能力**，仅照片比对）。

### 4. Android 前端（`fronted/`）

```bash
cd fronted
./gradlew :app:assembleDebug        # JDK 21 + Android SDK Platform 36.1；Windows 用 gradlew.bat
node --test app/src/test/js/        # 守卫测试：防止已退役的 CC-C 摄像头与 FFmpeg 依赖回潮
```

> `local.properties` 由 Android Studio 生成，不入库。退役的 CC-C Wi-Fi 摄像头测肤链路与全部 FFmpeg/`libwificamera` 原生库已移除，保留 CameraX 测肤、K7 蓝牙配网与 Filament 3D。

### 5. AI Coding 日志刷新与校验

```bash
python tools/export_project_logs.py
python tools/official-validator/tools/validate-log.py logs/
```
## 五、AI Coding 使用说明

本作品从需求拆解到验收全程由 AI 协作完成，**完整对话日志见 [`logs/`](logs/)**（截至 2026-09-20：4 位成员、436 个会话、297,761 条事件，均通过官方 `validate-log.py`）。

**协作方式**

- **需求拆解与方案设计**：以 `backend/doc/` 的设计文档与 OpenAPI 契约为唯一出处，先把契约、数据模型与错误码冻结，再拆成 A–E 五个工作包（`backend/doc/tasks/panels/`），每包有独立的任务清单与验收口径。
- **编码**：Codex 作总协调与监督，OpenCode 多 worker 并行实现各工作包；板端驱动移植沉淀为可复用 Skill（`skills/k7-openvela-driver-port`），视觉侧沉淀 `skills/skin-vision-toolkit`。仓库根的 `AGENTS.md` 约束 AI 的工作区边界、分支流程与"设计变更单独提交"。
- **调试与评审**：每个增量由**独立 Oracle 评审**多轮裁定（FAIL → IMPORTANT/BLOCKER 逐条闭合 → PASS），结论与探针复跑记录写入 `backend/handoffs/`；`backend/acceptance/` 的 94 场景黑盒框架做双跑取证，区分 `passed` / `device_pending` / `seam`，不用编译通过冒充真机验证。
- **文档**：交付报告、失败记录与交接说明同样由 AI 起草、人工核定，`docs/` 保留失败与回退记录不做美化。

**AI 带来的实际帮助（可核查的例子）**

- **质量**：Oracle 多轮评审在合入前拦下多个 BLOCKER（如跨语言 JCS 规范化的字节级差异、媒体策略默认拒绝、租约代次缺失导致的重复落地），并强制"编译通过 / RAM 加载 / 真机验证"分开表述，避免把配置项当成验收结论。
- **效率**：94 场景验收矩阵与证据自动归档（`backend/acceptance/`，含逐场景 HTTP 往返与数据库断言）由 AI 驱动重复执行，人工只看结论与差异。
- **本次提交过程中的两个真实修复**：
  1. 官方要求 Rebase and merge，而开发历史含 18 个 merge commit（其中 2 个把冲突解决只存在 merge 里），GitHub 试算 `rebaseable=false`。AI 定位到具体 3 个冲突文件并做线性化 + 压缩，PR #4/#6/#8 顺利合并；
  2. 发现组委会归集工具按 `session_id` 覆盖写、而 Codex 每次 resume/fork 会新开 rollout 且沿用父会话 ID，导致 4 个会话的已提交日志窗口被覆盖、若干段从未导出。用官方适配器重导并合并全部段落后，`logs/allenxun` 的事件数从 69,050 补到 101,741，且逐文件证明"旧事件一条不少"，再用组委会原版 `verify.py` 全量校验通过。

**日志采集与校验口径**：由组委会归集工具（本仓 `tools/export_project_logs.py` 与官方 validator）导出，按成员分目录、按真实北京时间分日，每目录一份 `manifest.json` 记录会话、事件数与文件路径；采集边界（如仅明文文本与工具调用、不含图像与加密推理）在每个会话条目的 `data_completeness_warning` 中如实标注。
