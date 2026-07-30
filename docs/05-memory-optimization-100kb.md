# 05 - 100KB 内存优化技术路线（含 TLS/证书优化）

> 从当前代码实现直接总结的优化路线。面向开发者与其他 AI：
> 每条都标注「现状 → 改动 → 验证方式 → 对应代码位置」。
> 另附与原 WS63 (goldieos) 实现的对照，便于移植理解。

## 0. 优化目标与结论

convai 子系统（WS 客户端 + TLS + 编解码 + 全部音频缓冲 + 任务栈）内存 **< 100KB**，
且网络/系统卡顿时不崩溃（丢帧不丢命）。

**最终预算（单一事实源：`components/convai_ws/include/convai_limits.h`）**

| 池 | 大小 | 代码位置 |
|---|---|---|
| 静态工作缓冲（编码 1KB + 解码 4KB） | 5 KB | convai_ws.c `s_enc_buf`/`s_dec_pcm` |
| TX 上行帧队列 12×772B | 9 KB | `s_tx_storage`（xQueueCreateStatic） |
| RX 下行抖动消息环 | 20 KB | `s_rx_arena` + convai_ring.c |
| 任务栈（ws 4K + 泵 3K + 发送 3K） | 10 KB | convai_limits.h |
| 编解码实例峰值（Opus enc+dec，懒加载） | 33 KB | codec_opus.c |
| 引擎 + JSON 工作集 | 3 KB | — |
| **子系统合计（明文 ws）** | **82 KB** | ✅ 测试断言 |
| + TLS 瘦身（in/out 各 4KB + 握手） | +12 KB | sdkconfig.defaults |
| **子系统合计（wss）** | **94 KB**（102400 内） | ✅ 测试断言 |
| + AItalk 应用核心（4×512 + 状态） | +2.1 KB | components/aitalk |
| **应用总计 ws / wss** | **84 KB / 96 KB** | ✅ `test_aitalk_memory_and_total_budget` |

验证：`host_tests` 用例 `test_subsystem_budget_under_100kb` 与
`test_aitalk_memory_and_total_budget` 用同一组 `convai_limits.h` 常量断言并打印上表（40/40 通过）。

## 1. 优化技术路线（按执行顺序）

### 路线 1：消除每帧 malloc（确定性基石）

- **现状**：早期实现每帧 `malloc/free`（编码 buf、解码 buf），长期运行产生碎片与不可预测峰值。
- **改动**：文件级静态数组 `s_enc_buf[1024]`（覆盖 20ms 最大编码输出 640B）、
  `s_dec_pcm[2048]`（解码样本封顶，超限帧直接丢弃+日志）。
- **代码**：`components/convai_ws/convai_ws.c` 顶部 static 区；
  边界由 `convai_limits.h` 的 `CONVAI_ENC_BUF_BYTES/CONVAI_DEC_PCM_SAMPLES` 统一定义。
- **验证**：`test_frame_encode_fits_static_buffer`——5 种格式 20ms 帧编码 ≤1024B、
  解码 ≤2048 样本（回归防溢出）。
- **对照 WS63**：goldieos `convai_bridge.c` 用 `g_pcm_decode_buf[1024]` 静态数组 +
  `PLAYBACK_RING_SIZE 8000` 静态播放环（`g_ring_data`）——同一思路，我们只是把它显式化、预算化。

### 路线 2：Opus 懒加载 + 定点 CELT

- **现状**：Opus enc 14.5KB + dec 18.4KB 是编解码最大项；SILK 路径有崩溃 bug（见 docs/04 坑 6）。
- **改动**：① `init` 不建实例，首次 encode/decode 才创建（半双工场景不同时占用）；
  ② 用 `OPUS_APPLICATION_RESTRICTED_LOWDELAY`（纯 CELT，定点）；
  ③ `mem_usage()` 用 `opus_encoder_get_size()` 精确记账，`deinit()` 对称释放。
- **代码**：`components/convai_ws/codec_opus.c`。
- **验证**：`test_memory_opus_under_64kb`（每侧 <64KB）、`test_opus_encode_decode_roundtrip`（10 帧流能量比 0.948）。

### 路线 3：卡顿弹性——静态池 + 丢帧不崩溃

- **现状**：单缓冲在网络/系统卡顿时只能阻塞或裸丢帧，且行为不可观测。
- **改动**：
  - **上行 TX 队列**（16 帧×768B，静态 xQueue）：录音线程只做 20ms 切片+编码+入队；
    内置发送任务消费。队列满 → **丢最老帧 + 计数 + 高水位**，录音永不阻塞、永不 OOM。
  - **下行 RX 消息环**（24KB，自研 `convai_ring`）：WS 收帧→解码→入环；
    播放泵任务消费（应用播放可慢，环吸收）。满 → **丢最老"整帧"**——
    ADPCM/Opus 解码器状态连续，丢字节必须按整帧丢（4B 头+负载为一个消息），
    否则解码流错位（这是实现时的关键坑）。
- **代码**：`convai_ws.c`（tx_push/send_task_fn/rx_push/pump_task_fn）、
  `convai_ring.c/h`（纯 C，可主机测试）。
- **验证**：`test_ring_*`（回绕/整帧丢弃/卡顿模拟/超限消息），
  `test_ring_stall_simulation` 模拟 2× 速生产：水位升→计数→恢复后序列连续。
- **Kconfig 弹性**：`CONVAI_TX_QUEUE_FRAMES`(16)、`CONVAI_RX_RING_KB`(24)、
  `CONVAI_WS_TASK_STACK`(4096)——内存富裕可调大，弱板可调小。
- **对照 WS63**：goldieos 播放环是单消费者 PRIMING/PLAYING 三态机
  （`convai_bridge.c:230`），阈值 480B 预充；我们保留同样的"先缓冲后播放"思想，
  但把环下沉到引擎内、加整帧丢弃语义与水位统计。

### 路线 4：TLS/证书优化（wss 场景省 40KB+ 且握手快 10 倍）

- **现状**：mbedTLS 默认 in/out 各 16KB 内容缓冲（wss 时子系统直接 +40KB）；
  RSA-2048 证书在 Xtensa 上握手昂贵。
- **改动**：
  1. **TLS 缓冲瘦身**：`sdkconfig.defaults` 设
     `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`、`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096`
     （convai 最大帧 <1KB，4KB 裕量充足）→ 省 24KB。
  2. **证书选 ECDSA P-256，不用 RSA-2048**：Xtensa 无大数指令，
     ECDSA 握手快约 10 倍、RAM 更小（生成脚本见网关仓库 `scripts/gen_cert.sh`）。
  3. **套件选 AES-256/128-GCM**：ESP32-S3 有 AES 硬件加速；
     避免 ChaCha20（纯软件耗 CPU）。网关侧已限定 CipherSuites 并优先 ECDSA。
  4. **TLS 1.2 + ECDHE P-256**：握手只发生一次（WS 长连接），P-256 标量乘几十 ms 可接受。
  5. 自签证书嵌入：`components/convai_ws/certs/server_ca.pem` 存在即自动
     `target_add_binary_data` 嵌入固件验签（`CONVAI_WSS_CUSTOM_CA`）；
     公共 CA 走内置证书包 `esp_crt_bundle_attach`；IP 直连可 Kconfig 跳过 CN 校验。
- **代码**：`convai_ws.c convai_start()` wss 分支、`components/convai_ws/Kconfig`、
  `sdkconfig.defaults`。
- **验证**：wss 预算 101KB<102400（测试断言）；证书性能数据见上表分析。
- **对照 WS63**：goldieos 的 TLSAL 是 stub（`convai_platform_ws63.c` 未实现 TLS，
  走明文）——我们补上了生产可用的 wss 路径并按 MCU 特性选了参数。

### 路线 5：可观测性（调优闭环）

- `convai_mem_report()`：静态池、codec 实例、TX/RX 水位与丢帧数、free heap/min-ever。
  上板跑一次即可判断：RX 水位贴顶 → 加大 `CONVAI_RX_RING_KB`；TX 丢帧 → 查网络而非加内存；
  RX 丢帧 → 播放线程提速或降码率。
- **代码**：`convai_ws.c convai_mem_report()`，`main/main.c` demo 每秒调用。

## 2. 端云 E2E 验证（mock 法）

**架构**：真实设备代码（convai_ws 组件，linux target 编译）↔ 真实网关（go-esp32-ws-server）
↔ mock 后端（`cmd/mockbackends`： canned ASR 500ms 出句、LLM 固定回复、TTS 0.8s 正弦）。

**结果**（`e2e_tests` 工程，两个场景全过）：

| 场景 | 结果 | 关键数据 |
|---|---|---|
| g711a (8k) | PASS | hello→listening→thinking→text 回复→answering→40 帧/12800B(0.8s)→answer_finished |
| opus (16k) | PASS | 同上，40 帧/25600B，opus 双向编解码线上验证 |

运行方式：`e2e_tests` 构建 linux target + 启动 mockbackends(51051/51052/51061) + router(:9000)。

## 3. 与原 WS63 (goldieos) 代码对照表（供移植学习）

| 主题 | WS63 原实现 | 本工程对应 | 差异说明 |
|---|---|---|---|
| G.711A 编解码 | `sdk_integration/convai_codec_g711a.c` | 同名文件（直接复用） | 零改动，测试向量一致 |
| 播放缓冲 | `convai_bridge.c` 环形缓冲+三态机(230-356行) | `convai_ring.c` + 泵任务 | 增加整帧丢弃/水位/丢帧计数 |
| 录音上行 | `audio_record_thread`(133-212行) PCM→G711A | `convai_send_audio` 切片+TX 队列+发送任务 | 格式可切换；队列替代直接发送 |
| 引擎生命周期 | `convai_bridge_start/stop` | `convai_start/stop` | 真实 WS 传输替代 SDK 内部 |
| 平台层 | `platform/convai_platform_ws63.c`(OSAL/NetAL/TLS stub) | FreeRTOS/esp_websocket_client/esp-tls | TLS 从 stub 变为完整实现 |
| 配置 | `convai_config.c` key=value | Kconfig + convai_limits.h | 编译期定界 |
| 人设下发 | `DEFAULT_STARTUP_CONFIG` (428-444行) | `main.c STARTUP_CONFIG` 一致 | config_update 协议相同 |

## 4. 后续可选优化（未做，列出路线）

1. **组件级内存池**：把 codec 状态（Opus 手工内存 `opus_encoder_init`）+ cJSON
   （`cJSON_InitHooks`）纳入编译期 arena，构造性硬上限（当前已是静态定界，此项为增强证明力）。
2. **sdkconfig 精简**：关闭 BT/LCD/camera/fatfs 等无关组件——省的是**整机** RAM/flash，
   不影响 convai 子系统预算。
3. **TTS 流式**：网关改流式后，RX 环可降到 8-12KB，把余量让给 TX 队列。
4. **任务栈按 watermark 精调**：`convai_mem_report` 输出 HWM 后收缩三个任务栈。
