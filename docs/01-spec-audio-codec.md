# 01 - 需求规格：音频编解码架构（Spec）

> SDD 阶段：规格（What & Why）。本文只描述"要做什么"，不涉及"怎么做"。

## 1. 背景

原 goldieos 固件（海思 WS63）的音频链路依赖**预编译 RISC-V 版 libconvai_sdk.a**，
无法用于 ESP32-S3（Xtensa）。因此需要在 ESP32 上用 WebSocket 重新实现 ConvAI
设备端 SDK，其中音频编解码子系统必须满足：

1. 云端（cloud_gateway）和设备可以协商音频格式；
2. 设备端算力有限（Xtensa LX7 @240MHz，无专用 DSP 指令集可用），编解码 CPU 开销必须可控；
3. 后续可能接入不同能力的网关/服务器，编解码格式必须**运行时可切换**，不能编译期写死。

## 2. 术语

| 术语 | 含义 |
|---|---|
| convai.v1 | cloud_gateway 的 WebSocket 线协议（JSON 文本信封 + 13 字节头二进制音频帧） |
| 编解码器 / codec | PCM16、G.711A、G.711U、IMA-ADPCM、Opus 之一的音频压缩算法实现 |
| 帧（frame） | 一次发送的音频单元，语音场景通常为 20ms |
| 定点 / FIXED_POINT | 纯整数运算的 Opus 构建模式，不使用浮点指令 |
| 懒加载 | 编码器/解码器实例在首次使用时才分配内存 |

## 3. 功能需求

### FR-1 多格式支持

系统必须内置以下编解码器，并给出统一的线上编号（写入 hello 消息 `audio_codec` 字段）：

| 编号 | 格式 | 压缩比 | 采样率 | 定位 |
|---|---|---|---|---|
| 0 | PCM16 | 1:1 | 8 kHz | 无压缩基线 |
| 1 | G.711A | 2:1 | 8 kHz | 默认（与原 ConvAI SDK 一致） |
| 2 | G.711U | 2:1 | 8 kHz | μ-law，互通性 |
| 3 | IMA-ADPCM | 4:1 | 8 kHz | 极低算力场景 |
| 4 | Opus | ~10:1 | 16 kHz | 高质量低带宽（定点实现） |

### FR-2 运行时动态切换

- 提供 `convai_set_codec(engine, codec_id)` API，调用后**立即生效**（下一帧起用新格式）。
- 每个编解码器有独立实例状态（如 ADPCM 的 predictor、Opus 的编码器句柄），
  切换时必须正确释放旧状态、重建新状态，不得泄漏。
- 提供 `convai_get_codec(engine)` 查询当前格式。

### FR-3 协议协商

- 设备在 WS 连接建立后发送的 `hello` 中携带当前 `audio_codec` 编号和 `sample_rate`。
- 网关可在 `hello_ack.audio_config.codec` 中以字符串（如 `"opus"`）要求切换，
  设备应自动遵从（若该格式已编译进固件）。

### FR-4 统一 PCM 界面

- 上行：应用层永远提交 **mono PCM16**；SDK 按当前编解码器编码后发送。
  （若应用已按当前格式预编码，可透传。）
- 下行：SDK 收到二进制音频帧后解码为 **mono PCM16** 再回调应用层。

### FR-5 Opus 可裁剪

Opus 体积约 200KB，必须通过 Kconfig 选项（`CONFIG_CONVAI_ENABLE_OPUS`）可整体裁掉；
裁掉后 `convai_set_codec(OPUS)` 返回 `CONVAI_ERR_NOT_SUPPORTED`，不影响其他格式。

## 4. 非功能需求

### NFR-1 内存预算

**每个编解码器的编码侧或解码侧实例内存 < 64KB**（堆分配，含内部状态）。
编码与解码不得强制同时占用内存（懒加载）。

### NFR-2 算力预算

- G.711A/U、IMA-ADPCM：纯查表/整数运算，单帧（20ms）处理时间 << 1ms，适合最弱 MCU。
- Opus：定点（FIXED_POINT）构建，禁用浮点 API、汇编和 SIMD intrinsics；
  复杂度限制为 1，16kHz 单声道 16kbps。

### NFR-3 可测试性

- 编解码层和协议编解码层必须是**纯 C、平台无关**（不依赖 FreeRTOS/网络），
  以便在 PC（ESP-IDF linux target）上跑单元测试。
- 每个编解码器必须有「编码→解码」往返测试用例。

## 5. 验收标准（对应测试用例）

| # | 验收项 | 验证方式 | 状态 |
|---|---|---|---|
| AC-1 | 5 种格式注册可查询（按 id/名称/遍历） | `test_registry_*` | ✅ |
| AC-2 | PCM16 encode→decode 完全一致 | `test_pcm16_passthrough` | ✅ |
| AC-3 | G.711A 静音=0xD5、G.711U 静音=0xFF；往返保号、误差在量化步长内 | `test_g711*` | ✅ |
| AC-4 | IMA-ADPCM 精确 4:1 压缩；正弦往返收敛后有界误差 | `test_ima_adpcm_*` | ✅ |
| AC-5 | Opus 10 帧连续流 encode→decode 帧长正确、稳态能量比 ≈0.95 | `test_opus_*` | ✅ |
| AC-6 | 四格式动态切换全流程（建状态→编码→解码→释放）无泄漏无崩溃 | `test_dynamic_codec_switch` | ✅ |
| AC-7 | 每种格式编码侧/解码侧内存 < 64KB（运行时打印并断言） | `test_memory_*` | ✅ |
| AC-8 | esp32s3 固件编译链接通过 | `idf.py build` | ✅ |

实测内存：PCM/G711 = 0B，ADPCM = 16B，Opus 编码 14,484B / 解码 18,420B。
