# 06 - 从 WS63 到 ESP32：端侧 SDK 迁移与适配说明

> 本文说明 goldieos（海思 WS63）端侧 AI 对话能力迁移到 ESP32-S3 的
> 背景、架构映射和逐步迁移方案。读者：需要理解"为什么这样改"的开发者，
> 或需要把其余模块（UI/驱动/服务）继续搬过来的 AI。

## 1. 适配背景与意义

### 1.1 原平台（WS63）的构成

goldieos 是儿童 AI 玩具"小荷"的固件，核心链路：

```
唤醒词 → ConvAI SDK(libconvai_sdk.a) ──WSS──▶ 云端 ASR/LLM/TTS
         ↑ 预编译 RISC-V/musl 静态库，闭源
ES8311 录音/播放 ← G.711A 8kHz ↔ 云端
```

关键事实：

| 项 | WS63 现状 | 对迁移的影响 |
|---|---|---|
| ConvAI SDK | **预编译 RISC-V 库**，无源码、无 Xtensa 版本 | 无法用任何方式直接链接到 ESP32 |
| 线协议 | SDK 内部实现，但官方文档 `cloud_gateway/docs/protocol.md` 完整公开（convai.v1） | **协议可重写**，这是迁移的合法路径 |
| OS | LiteOS + 自研 goldie_osal | 全部需换 FreeRTOS |
| 网络 | lwIP + mbedtls（TLSAL 是 stub，实际明文 ws） | ESP-IDF 有现成 esp_websocket_client + esp-tls |
| 音频 | ES8311 codec + I2S，8kHz PCM，G.711A 上行 | ESP32 常用同款 codec；G.711A 代码纯 C 可直接复用 |
| 唤醒 | 外部 ASR 芯片 AC2817 / TFLM KWS | ESP-SR（WakeNet/AFE）是 S3 原生替代 |
| 星闪 SLE | 对讲机功能 | ESP32 无星闪，需改 BLE/ESP-NOW 或裁剪 |

### 1.2 为什么值得迁移

1. **供应链与成本**：ESP32-S3 供应稳定、单价低、生态大（ESP-IDF/ESP-SR/LVGL 全开源）。
2. **摆脱闭源库绑定**：协议公开，重写后端侧代码 100% 自主可控，可自由扩展
   （多编解码、缓冲策略、TLS 参数），不再受 SDK 版本和平台库限制。
3. **调试与测试能力**：ESP-IDF 支持 **linux target 主机端单元测试**——
   WS63 上只能上板调试，现在协议/编解码/缓冲全部可在 PC 上跑 35 项测试 + 端云 E2E。
4. **性能相当**：S3 双核 240MHz + 向量指令 + AES 硬件加速，定点 Opus/CELT、
   G.711、ADPCM 均实时富余。

### 1.3 迁移原则

- **协议不变**：convai.v1 线上格式逐字节兼容，云端/网关零改动。
- **API 不变**：保留原 SDK 的 `convai_create/start/stop/update/send_audio/send_message`
  及四回调（event/status/audio/message），上层应用逻辑（AItalk 状态机）可平移。
- **逐层验证**：每迁移一层先写主机测试，再上板——杜绝"最后一次性联调"。

## 2. 整体架构（迁移后）

```
┌──────────────────────── ESP32-S3 固件 ─────────────────────────┐
│ 应用层  main.c（demo）/ 未来 AItalk App（平移 WS63 状态机）      │
├────────────────────────────────────────────────────────────────┤
│ convai_ws 组件（= 原 libconvai_sdk.a + convai_bridge.c 的重写） │
│  ├ convai_api      与原 SDK 相同的生命周期/回调                  │
│  ├ 引擎            WS 事件、hello 协商、状态机、任务编排          │
│  ├ 协议层          convai_protocol.c（信封/13B音频头，纯C）      │
│  ├ 编解码注册层    PCM16/G711A/G711U/IMA-ADPCM/Opus 动态切换     │
│  └ 缓冲层          TX 队列 + RX 消息环（静态池，卡顿丢帧不崩溃） │
├────────────────────────────────────────────────────────────────┤
│ 平台层  esp_websocket_client / esp-tls / FreeRTOS / cJSON       │
│         （替代 goldie_osal + lwIP sockets + mbedtls stub）       │
├────────────────────────────────────────────────────────────────┤
│ 第三方  opus-1.6.1（定点 CELT，从 goldieos third_party 平移）    │
└────────────────────────────────────────────────────────────────┘
              │ convai.v1（WS 子协议，与 WS63 完全同协议）
              ▼
        cloud_gateway / go-esp32-ws-server → ASR/LLM/TTS
```

**映射关系（一句话版）**：`libconvai_sdk.a(闭源)` → `convai_ws 组件(开源重写)`；
`convai_bridge.c(桥接/缓冲)` → 引擎内缓冲层；`goldie_osal(LiteOS)` → FreeRTOS 原生 API；
`convai_platform_ws63.c(平台适配)` → ESP-IDF 组件直接提供，无需适配层。

## 3. 逐步迁移方案

### 阶段 0：环境与基线（已完成）

- WSL2 + ESP-IDF v6.2（EIM 安装），hello_world 针对 esp32s3 编译通过。
- 产出：可用工具链、`get_idf` 激活别名。
- **教训**：EIM 的 activate 脚本不能在子 shell 里 source（检测 $0），
  脚本化构建用 `export IDF_PYTHON_ENV_PATH=... + . export.sh`。

### 阶段 1：协议逆向与规格化（已完成）

- 从 `libconvai_sdk.a` 提取字符串确认线协议轮廓（session.* / input_audio_buffer.* /
  G.711A / base64），再对照官方 `cloud_gateway/docs/protocol.md` 锁定 convai.v1 精确格式：
  文本信封 `{type,seq,ts,body}` + 二进制 13B 大端头（op/seq/ts）+ G.711A。
- 产出：`docs/01-03`（规格/设计/实现指南），协议层 `convai_protocol.c`（纯 C 可测）。

### 阶段 2：纯 C 层迁移（已完成，零平台依赖）

按"最容易复用 → 最新写"顺序：

| 模块 | 来源 | 处理 |
|---|---|---|
| G.711A 编解码 | goldieos `convai_codec_g711a.c` | **原样拷贝**（纯 C 查表） |
| 播放环思想 | `convai_bridge.c` 环形缓冲+PRIMING | 重写为 `convai_ring.c`（消息环+整帧丢弃+水位） |
| 配置 JSON 结构 | `convai_bridge.c` bridge_build_config_json | 保留 info/ws 结构，值改 Kconfig |
| G.711U / IMA-ADPCM / PCM16 | 新增 | 同接口实现，测试向量与网关对齐 |
| Opus | goldieos `third_party/opus-1.6.1` | 源码平移，定点构建（**发现 SILK 编码器崩溃 bug，改纯 CELT**） |

- 验证：linux target Unity 测试（编码/解码/信封/音频头），
  **准则：有损格式不逐样本比，用多帧稳态能量比**。

### 阶段 3：平台层替换（已完成）

| WS63 | ESP32-S3 替代 | 说明 |
|---|---|---|
| goldie_thread_create/sem | xTaskCreate/xSemaphore | 泵任务、发送任务 |
| mbedtls_net_* socket | esp_websocket_client | 含重连、子协议 |
| TLSAL stub（未实现） | **esp-tls + crt_bundle** | wss 完整实现，超越原平台 |
| WiFiService(预编译库) | esp_wifi + event group | demo 用 menuconfig 配网 |
| LiteOS 主循环 | app_main + FreeRTOS 任务 | — |

### 阶段 4：引擎与缓冲（已完成）

- `convai_ws.c`：hello 协商 → session_ready → config_update 下发人设；
  status/event/text/function_call 分发到与原 SDK 相同的回调。
- 缓冲：TX 队列（网络卡顿丢最老帧）+ RX 消息环（播放卡顿丢最老**整帧**）。
- 内存：<100KB 静态定界（见 docs/05），`convai_mem_report()` 可观测。

### 阶段 5：联调验证（已完成）

- 主机单元测试 35 项全过；esp32s3 固件编译通过。
- mock 端云 E2E：真实 convai_ws（linux target）↔ 真实网关 ↔ mock ASR/LLM/TTS，
  g711a/opus 两场景全过（hello→说→thinking→text→TTS 帧→answer_finished）。
- 下一步：上板（usbipd 透传串口）+ 真实 ASR/TTS（本机 :50051/:50061 已在跑）联调。

### 阶段 6：尚未迁移的模块（后续路线图）

| 模块 | 迁移方案 | 优先级 |
|---|---|---|
| ES8311 录音/播放 | ESP-IDF 官方 es8311 组件或平移 goldieos `drivers/codec/es8311_drv` 寄存器表 | P0（上板出声） |
| 唤醒词 | ESP-SR WakeNet（"你好小荷"需重训模型）或保留外部 ASR 芯片 GPIO | P1 |
| AItalk 应用状态机 | 平移 `apps/AItalk/main_app.cpp` 逻辑（SDK 回调签名已兼容） | P1 |
| GUI | tiny_gui(闭源) → LVGL；位图/中文字库资源可数据级复用 | P2 |
| 闹钟/NTP | alarm_service 逻辑平移；ntp_service → esp_sntp | P2 |
| 对讲机（星闪） | ESP-NOW 或 BLE 重写，或裁剪 | P3 |
| 电源/电量 | aw9523b/bat_driver 按新硬件重写 | P2 |

## 4. 迁移经验沉淀（给后续迁移者的清单）

1. **先找协议文档，再做字符串逆向兜底**：有 `protocol.md` 就不要猜；
   没有文档时 `strings libxxx.a | grep -E 'ws://|session|json'` 是最快的轮廓提取法。
2. **纯 C 先行**：协议编解码、G.711、ADPCM、环形缓冲全部平台无关，
   先让它们通过主机测试，再碰任何 ESP-IDF API。
3. **第三方库"编译过 ≠ 能跑"**：vendored opus-1.6.1 编译一路绿灯，
   运行才暴露 SILK 编码器堆越界（watchpoint 定位，见 docs/04 坑 6）。
   每个第三方路径都要有"编码→解码"真实数据验证。
4. **API 兼容层值得做**：保持 `convai_*` 签名与原 SDK 一致，
   上层应用（AItalk 的 467 行状态机）才能零改动平移。
5. **内存用"静态定界 + 丢帧计数 + 水位观测"三件套**，比"尽量省"更工程化
   （docs/05 有完整路线）。
6. **E2E 不必等硬件**：linux target 能跑真实的网络栈（esp-tls/socket），
   端侧代码在 PC 上联调网关，把上板联调压缩到只剩硬件驱动问题。
