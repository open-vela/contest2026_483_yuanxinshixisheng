# R49 第三方许可与资产来源

更新：2026-11-04。对应镜像 `3479d1bd…`，33,548,288字节。

这是依赖许可与来源清单。第三方原文、文件级声明和未知项均保留；尚不能声明整个R49镜像已获Apache-only或组合二进制分发许可。根 `LICENSE` 仅按根 `NOTICE` 所述覆盖本项目有权授权的原创部分。

原文索引：[licenses/INDEX.json](licenses/INDEX.json)。机器可读完整清单：[THIRD_PARTY_NOTICES.json](THIRD_PARTY_NOTICES.json)。其中仅列R49相关依赖与明确标注的历史可选项目，没有把旧legacy/WIP整体列为运行依赖。

| 组件 | 版本/身份 | 原许可 | 核对边界 |
| --- | --- | --- | --- |
| VelaVision 原创部分 | R49/source49-typed-tail-recovery-r1 | Apache-2.0 | 根 LICENSE 不覆盖第三方与派生作品；不把全部镜像改标 Apache。 [官方来源1](https://github.com/open-vela/contest2026_483_yuanxinshixisheng)；[原文](licenses/Apache-2.0.txt) |
| openvela / Apache NuttX | e987a81c32cab008d1a8521669e5488d00271322 + 记录中的26项本地tracked改动 | Apache-2.0 AND file-level exceptions | 原 NOTICE 全文随包；不能把当前SDK改动自动等同历史全部父对象源码。 [官方来源1](https://github.com/open-vela/nuttx/tree/e987a81c32cab008d1a8521669e5488d00271322)；[原文](licenses/sdk/nuttx/LICENSE)；[原文](licenses/sdk/nuttx/NOTICE) |
| openvela nuttx-apps | 92cc8c56a4c9b780b38991401295179fa2461908 | Apache-2.0 AND file-level exceptions | 版本是wrapper仓库HEAD，不充当嵌套第三方库的上游版本。 [官方来源1](https://github.com/open-vela/nuttx-apps/tree/92cc8c56a4c9b780b38991401295179fa2461908)；[原文](licenses/sdk/apps/LICENSE)；[原文](licenses/sdk/apps/NOTICE) |
| Seekwave 无线协议派生代码 | 上游驱动完整revision未闭合；本地文件哈希已扫描 | GPL-2.0-only | 保留Seekwave版权与GPL声明。R49含Apache与GPL2-only组合，组合分发条件/赛事Apache要求未由本清单解决。 [官方来源1](https://doc.kickpi.com/products/download/download/)；[原文](licenses/project/seekwave/COPYING.GPL-2.0) |
| Rockchip 显示/时钟/电源派生代码 | RK3576 Linux SDK 24a411114a2e4e3db89acdf15271aa7a32a8590d；各blob见来源文档 | GPL-2.0-only AND GPL-2.0-or-later | 各文件版本选择保持原样。不能以寄存器事实为由重标整个派生驱动。 [官方来源1](https://doc.kickpi.com/products/download/download/)；[原文](licenses/project/seekwave/COPYING.GPL-2.0) |
| charsiu NPU诊断夹具（范围待核） | 2ac3dc8ef6221a37d348c967b1cce29c01d225ff | GPL-2.0-or-later | 不算作已证实R49生产模型；如发布该源码仍保留原许可。 明确来源链接待补；[原文](licenses/historical/charsiu-LICENSE) |
| ggml（llama.cpp b5046快照） | 74d4f5b041ad837153b0e90fc864b8290e01d8d5 | MIT AND (Apache-2.0 WITH LLVM-exception) | ggml一般MIT；SDK快照27份可选SYCL源码另保留Apache-2.0 WITH LLVM-exception及Intel/LLVM版权，逐文件见映射。没有宣称SYCL已在R49运行或加速；不把历史llama/WIP全部算入R49。 [官方来源1](https://github.com/ggml-org/llama.cpp/tree/74d4f5b041ad837153b0e90fc864b8290e01d8d5)；[原文](licenses/ggml/LICENSE)；[原文](licenses/sdk/nuttx/libs/libxx/libcxx/libcxx/LICENSE.TXT) |
| wpa_supplicant / hostap | 2.12；官方tar SHA08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6 | BSD-3-Clause | 官方同SHA包的README/COPYING已提取，保留逐文件声明；旧BSD/GPL双选历史不推定当前代码可任意换标。 [官方来源1](https://w1.fi/releases/wpa_supplicant-2.12.tar.gz)；[原文](licenses/wpa_supplicant-2.12/README)；[原文](licenses/wpa_supplicant-2.12/COPYING)；[原文](licenses/wpa_supplicant-2.12/LOCAL-COPYING) |
| cJSON | 1.7.15（本地头版本宏） | MIT | 从现有源文件完整首版权/许可块逐字提取；非重写的通用MIT模板。 [官方来源1](https://github.com/DaveGamble/cJSON/tree/v1.7.15)；[原文](licenses/cJSON/LICENSE-extracted.txt) |
| WebRTC Audio Processing | 2.1（保留对象DWARF路径）；官方基线包SHA ae9302824b2038d394f10213cab05312c564a038434269f11dbf68f511f9f9fe | BSD-3-Clause AND bundled third-party licenses | 历史官方原包已找回并与本次官方包SHA一致；原文闭合。实际全源重建由独立构建验收说明。 [官方来源1](https://www.freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-2.1.tar.xz)；[原文](licenses/webrtc-audio-processing-2.1/COPYING)；[原文](licenses/webrtc-audio-processing-2.1/webrtc/LICENSE)；[原文](licenses/webrtc-audio-processing-2.1/webrtc/PATENTS) |
| PFFFT / FFTPACK相关代码 | WebRTC Audio Processing2.1内含；R49另有repair输入 | LicenseRef-PFFFT-FFTPACK | 历史修复C/H与来源manifest、preparation已找回并核对hash；保留FFTPACK/PFFFT原文，未重授权。 [官方来源1](https://www.freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-2.1.tar.xz)；[原文](licenses/webrtc-audio-processing-2.1/webrtc/third_party/pffft/LICENSE)；[原文](licenses/webrtc-audio-processing-2.1/webrtc_third_party_pffft_src_pffft.c.license-extract.txt) |
| RNNoise RNN VAD weights/code | WebRTC Audio Processing2.1内含 | BSD-3-Clause | 原文随包；独立外部录音和Silero模型不归此项。 [官方来源1](https://www.freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-2.1.tar.xz)；[原文](licenses/webrtc-audio-processing-2.1/webrtc/third_party/rnnoise/COPYING) |
| Abseil（APM所用） | 20240722.1（DWARF路径）；SDK副本另有自己的锁 | Apache-2.0 | 上游版本许可与SDK副本区分；实际裁剪/补丁闭包以构建证据为准。 [官方来源1](https://github.com/abseil/abseil-cpp/releases/download/20240722.1/abseil-cpp-20240722.1.tar.gz)；[原文](licenses/abseil-20240722.1/LICENSE)；[原文](licenses/sdk/external/abseil-cpp/abseil-cpp/AUTHORS) |
| Mbed TLS | 上游精确版本待实际版本头；wrapper=nuttx-apps92cc8c56 | Apache-2.0 | 不能将wrapper父仓HEAD冒充Mbed TLS上游revision。 [官方来源1](https://github.com/Mbed-TLS/mbedtls)；[官方来源2](https://github.com/open-vela/nuttx-apps/tree/92cc8c56a4c9b780b38991401295179fa2461908/crypto/mbedtls)；[原文](licenses/sdk/apps/crypto/mbedtls/mbedtls/LICENSE) |
| libsodium | 上游精确版本待实际版本头；wrapper=nuttx-apps92cc8c56 | ISC | ISC声明来自实际SDK原文，非Apache。 [官方来源1](https://github.com/jedisct1/libsodium)；[官方来源2](https://github.com/open-vela/nuttx-apps/tree/92cc8c56a4c9b780b38991401295179fa2461908/crypto/libsodium)；[原文](licenses/sdk/apps/crypto/libsodium/libsodium/LICENSE)；[原文](licenses/sdk/apps/crypto/libsodium/libsodium/AUTHORS) |
| Opus | 上游精确版本待版本头；wrapper=external740f9f5a | BSD-3-Clause AND file-level notices | 保留codec许可；如另有PATENTS文件以补充收据核对。 [官方来源1](https://opus-codec.org/)；[官方来源2](https://github.com/open-vela/external/tree/740f9f5a0f0106e1870ebfa2ad670a10644dec25/opus)；[原文](licenses/sdk/external/opus/opus/COPYING)；[原文](licenses/sdk/external/opus/opus/AUTHORS) |
| libjpeg-turbo / IJG / SIMD | 上游精确版本待实际版本头；wrapper=external740f9f5a | BSD-3-Clause AND IJG AND Zlib | 按实际LICENSE.md的复合说明保留，不统一简化成MIT/Apache。 [官方来源1](https://github.com/libjpeg-turbo/libjpeg-turbo)；[官方来源2](https://github.com/open-vela/external/tree/740f9f5a0f0106e1870ebfa2ad670a10644dec25/libjpeg-turbo)；[原文](licenses/sdk/external/libjpeg-turbo/libjpeg-turbo/LICENSE.md)；[原文](licenses/sdk/external/libjpeg-turbo/libjpeg-turbo/README.ijg) |
| LLVM libc++ / libc++abi | 上游精确版本待版本头；SDK nuttx e987a81c32c | Apache-2.0 WITH LLVM-exception | 原文件含LLVM例外及历史许可，全文保留。 [官方来源1](https://github.com/llvm/llvm-project)；[官方来源2](https://github.com/open-vela/nuttx/tree/e987a81c32cab008d1a8521669e5488d00271322/libs/libxx)；[原文](licenses/sdk/nuttx/libs/libxx/libcxx/libcxx/LICENSE.TXT)；[原文](licenses/sdk/nuttx/libs/libxx/libcxxabi/libcxxabi/LICENSE.TXT) |
| Android support code | external740f9f5a；嵌套上游精确revision未闭合 | Apache-2.0 AND file-level exceptions | 许可证包含相关SDK声明，但不是所有Android子项目均进入R49的断言。 [官方来源1](https://android.googlesource.com/)；[官方来源2](https://github.com/open-vela/external/tree/740f9f5a0f0106e1870ebfa2ad670a10644dec25/android)；[原文](licenses/sdk/external/android/system/core/LICENSE)；[原文](licenses/sdk/external/android/system/core/libcutils/NOTICE)；[原文](licenses/sdk/external/android/system/core/libutils/NOTICE)；[原文](licenses/sdk/external/android/system/libbase/LICENSE)；[原文](licenses/sdk/external/android/system/libbase/NOTICE) |
| GCC / libgcc runtime | 13.4.0；compiler SHA256 d80c929eca89733c50500a0e5288e181f14bff1cdd15e954e21b2bc142a72ead | GPL-3.0-or-later WITH GCC-exception-3.1 | 已收官方对应版本原文；例外适用条件保留，不据此推出整镜像许可已通过。 [官方来源1](https://gcc.gnu.org/releases.html)；[官方来源2](https://github.com/gcc-mirror/gcc/tree/releases/gcc-13.4.0)；[原文](licenses/gcc-13.4.0/COPYING3)；[原文](licenses/gcc-13.4.0/COPYING.RUNTIME) |
| 工具链 libm / Newlib runtime | GCC13.4工具链中的libm；实际Newlib版本/对应COPYING.NEWLIB待闭合 | NOASSERTION | SDK另存Newlib源码COPYING类文本是辅助资料，不能当作该预构建libm确切分发许可的证明。 [官方来源1](https://sourceware.org/newlib/)；授权原文尚缺 |
| YuNet 2023mar权重派生数组 | ONNX SHA8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4 | MIT | 官方模型目录许可原文已补齐；数组头提及的generate_yunet.py未在当前主仓找到，转换链/具体发布范围仍由资产锁说明，不误套OpenCV Zoo根Apache。 [官方来源1](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet)；[原文](licenses/models/yunet/LICENSE) |
| FSA-Net 头姿权重派生数组 | 上游ONNX d85e90cb…；本地处理后120fa107a2dd3be78c21c3a73a0db980590643a5372893e8878094898262f213 | MIT | 明确区分上游与处理后hash。上游提示训练数据集有各自许可；处理链与数据集/商业范围不作扩大授权。 [官方来源1](https://github.com/ohtlab/headpose-fsanet-pytorch/tree/002549c981b607f722e0810d999afcde7e5aaa66)；[原文](licenses/models/fsanet/LICENSE) |
| Silero VAD v5.1及本地运行库 | 模型symbol k7_vad_silero51_data SHA1807d455176de671fef6ff184b2a3f25f7e10e3f93ec61534334196644fd3873 | MIT | 官方v5.1 ONNX2623a295…经记录中的ORT1.17.1转换为1807d455…，现存转换文件2,421,160B与R49符号hash一致；保留MIT许可。 [官方来源1](https://github.com/snakers4/silero-vad/tree/84768cefdf5a3852400e9d8237f7315d14b64a08)；[原文](licenses/vad-local/LICENSE.silero)；[原文](licenses/models/silero-vad-v5.1/LICENSE)；[原文](licenses/vad-local/LICENSE.ort) |
| Seekwave SWT6621S 二进制固件/校准数据 | DRAM/IRAM/NV/calib各自SHA见assets条目 | NOASSERTION | 源路径与哈希不等于再分发授权；未找到能明确覆盖这些blob的许可，继续锁定。 [官方来源1](https://doc.kickpi.com/products/download/download/)；授权原文尚缺 |
| 云TTS生成本地提示/确认音 | qwen-audio-3.0-tts-flash；default/firefly标签；R49新增10条 | NOASSERTION | AI生成音频非自动Apache授权。官方系统音色longanhuan_v3.6可核；firefly来源/可用权利待核，官方基础音色目录精确ID未命中。 [官方来源1](https://help.aliyun.com/zh/model-studio/related-agreements)；[官方来源2](https://help.aliyun.com/zh/model-studio/qwen-audio-tts-voice-list)；授权原文尚缺 |
| recorded_reference / mic / residual录音夹具 | 各361,600B；SHA见R49 retention资产表 | NOASSERTION | 来源、说话人同意和可公开再分发范围未找到明确收据；不复制音频本体。 session_ab_pcm.inc等生成C/INC向量与原PCM按同样权利范围处理，不能因后缀而公开。 明确来源链接待补；授权原文尚缺 |
| 本地KWS Zipformer三模型及tokens | sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20；逐文件hash绑定 | Apache-2.0 | 模型卡明确Apache2；作者/维护者2026-10-08与10-26在官方issue3802确认两项KWS模型许可。记录转换后的ORT hash，模型本体未复制。 [官方来源1](https://modelscope.cn/models/pkufool/icefall-kws-zipformer-zh-en-3M-2025-12-20)；[官方来源2](https://github.com/k2-fsa/sherpa-onnx/issues/3802#issuecomment-6052663364)；[原文](licenses/Apache-2.0.txt)；[原文](licenses/models/kws/README.model-card.md) |
| Sherpa-ONNX 本地KWS/VAD运行库 | 历史记录1.12.14；实际构建补丁闭包另据锁核对 | Apache-2.0 | 使用官方对应版本许可原文；其内含依赖与模型各自独立列项。 [官方来源1](https://github.com/k2-fsa/sherpa-onnx/tree/v1.12.14)；[原文](licenses/sherpa-onnx-v1.12.14/LICENSE) |
| ONNX Runtime | 历史模型转换使用1.17.1；目标运行库完整源码身份另据构建锁 | MIT AND bundled third-party notices | 本地实际导出MIT原文与对应上游版本第三方全文均保留；不会把host wheel成功当目标源码闭合。 [官方来源1](https://github.com/microsoft/onnxruntime/tree/v1.17.1)；[原文](licenses/vad-local/LICENSE.ort)；[原文](licenses/onnxruntime-v1.17.1/LICENSE)；[原文](licenses/onnxruntime-v1.17.1/ThirdPartyNotices.txt) |
| SpeexDSP MDF / FFT wrapper | 发布源路径标记speexdsp-1.2.1；四TU来源按本地源hash绑定 | BSD-3-Clause | 完整保留Jean-Marc Valin等原版权/许可块；本轮只读本地源核对，未把目录版本自动当作上游tar等同性证明。 明确来源链接待补；[原文](licenses/speexdsp-1.2.1/mdf.c.license-extract.txt)；[原文](licenses/speexdsp-1.2.1/fftwrap.c.license-extract.txt) |
| KISS FFT（SpeexDSP内含） | SpeexDSP1.2.1内含副本；独立KISS版本未声明 | BSD-3-Clause | 按实际原文保留Mark Borgerding与Jean-Marc Valin版权；不混用新版本的KISS许可或笼统归为Speex单一版权。 明确来源链接待补；[原文](licenses/kissfft-speexdsp-1.2.1/kiss_fft.c.license-extract.txt)；[原文](licenses/kissfft-speexdsp-1.2.1/kiss_fftr.c.license-extract.txt) |

GPL逐文件映射：[R49-GPL-FILE-MAPPING.json](licenses/R49-GPL-FILE-MAPPING.json)。映射区分当前发布树与已确认相关的主仓依赖源。当前发布树有197条GPL文件路径（GPL-2.0-only 43，GPL-2.0-or-later 154）；冻结闭包与SDK快照重复路径不等于独立运行模块数。

发布的SDK快照另含27份带LLVM异常声明的ggml可选SYCL源：[逐文件声明与SHA](licenses/ggml/R49-SYCL-FILE-MAPPING.json)。源内Intel/LLVM版权保留，适用原文与一般MIT文件分开；文件存在不证明该后端在R49执行或加速。

## 生成音频与未公开资产

R49保留收据有62个继承资源；新增护理提示2条、手动档位/停止确认8条。清单保存符号、文本标签、哈希、编码/裁剪信息和来源收据，没有复制音频、模型、固件或私密配置本体。新增音频由 `qwen-audio-3.0-tts-flash` 云服务生成，运行时本地播放不等于资产自动成为Apache许可。

已核对官方系统音色列表中的 `longanhuan_v3.6`。另一个 `firefly` 仅为项目标签；生成器中的具体音色ID未命中本次官方基础音色目录，其属于公开基础音色、自定义复刻或设计音色尚无明确绑定凭据。不能仅凭命名判为侵权或推定已获授权。公开清单不披露该唯一音色ID。

官方百炼服务协议2026-09-02生效版本第7.5条对合法输入、使用自家模型的合成内容有带例外的归属约定；第4.3、4.4和专用条款仍适用。该协议不是开源音色许可，也不能代替具体音色或录音来源的权利凭据。参阅[官方相关协议入口](https://help.aliyun.com/zh/model-studio/related-agreements)与[官方音色文档](https://help.aliyun.com/zh/model-studio/qwen-audio-tts-voice-list)。

## 尚未关闭的分发事项

- **GPL_APACHE_COMBINATION**（R49组合二进制/赛事Apache-only表述）：无线和显示GPL-2.0-only派生部分真实存在；保留原license但没有替上游授予Apache或组合许可。需确认适用的组合分发方案/单独许可。
- **FIRMWARE_REDISTRIBUTION**（Seekwave DRAM/IRAM等blob）：有哈希，没有可复核的明确blob再分发条款。
- **GENERATED_AUDIO_VOICE_RIGHTS**（default/firefly生成提示与确认音）：生成服务/内容元数据已找到；firefly精确来源及适用权利仍待确认，不重授权为Apache。
- **RECORDED_FIXTURE_RIGHTS**（recorded_reference/mic/residual）：需来源与公开范围凭据；不复制原始录音。
- **SOURCE_AND_RUNTIME_BINDING**（工具链libm与部分目标依赖源码绑定）：APM原包/PFFFT修复与Silero转换记录已找回；这不等于全源clean重建或工具链libm许可身份已闭合，以构建锁和专项元数据为准。

Apache官方说明指出Apache-2.0与GPLv2存在兼容性问题；本清单据此保留组合分发事项，未擅自把GPL-2.0-only改成GPLv3或Apache。见[ASF兼容性说明](https://www.apache.org/licenses/GPL-compatibility.html)。

缺少授权/源码身份时，当前交付的是有明确边界的源码候选与许可资料，不用“第三方全部清理完成”描述。用户后续提供明确的上游许可、模型/音色来源或适用的分发方案后，应绑定证据更新清单。
