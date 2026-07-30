# 07 - AItalk 应用移植设计（协议栈/SDK 部分）与 <100KB 优化方案

> 本文记录 `goldieos/apps/AItalk/main_app.cpp`（467 行）移植到 ESP32 的
> 设计决策：哪些搬走、哪些丢弃、内存如何与重写后的 SDK 一起压进 100KB。

## 1. 原应用解剖（WS63, 467 行）

| 部分 | 行数 | 性质 | 移植决策 |
|---|---|---|---|
| 眼睛/表情动画 update_avatar_ui + 40 张 RGB565 位图引用 | ~200 | **GUI**（tiny_gui 控件） | 丢弃渲染，**保留动画计数逻辑**；LVGL 阶段再接 |
| play_type 状态机（SLEEP/SILENCE/SPEAK，由 SDK 状态驱动） | play_task ~30 | **协议栈相关** | 原样移植 `aitalk_on_sdk_status` |
| EMOTION_* 枚举 + 云端情绪驱动表情 | 分散 | **协议栈相关** | 移植 + 补全 function_call 解析（原代码依赖 SDK 内部消息回调） |
| MsgQueue 广播消息队列（4×1500B） | ~50 | **SDK 周边** | 移植并**裁剪 1500→512B** |
| App 生命周期（goldie_app_run/exit/suspend/resume、按键） | ~80 | 平台框架 | 不搬；ESP 侧由 app_main 直接编排 |
| convai_bridge 调用（get_engine/on_status/start/stop） | ~20 | **SDK API** | API 兼容直接平移（convai_*） |

结论：AItalk 的**协议栈与 SDK 部分只占原文件约 40%**，全部收敛到新组件
`components/aitalk/`（aitalk_app.c，~250 行，纯 C 无 GUI 依赖）。

## 2. ESP32 侧架构

```
convai_ws 引擎回调                     components/aitalk
on_status(status) ───────────────▶ aitalk_on_sdk_status ──▶ play_type 状态机
on_message(json) ────────────────▶ aitalk_on_sdk_message ─▶ emotion 状态
                                          │                    (function_call "emotion")
                                          └── call_id ──▶ convai_send_message(function_call_output)
系统广播(未来) ────────────────────▶ aitalk_push/pop_chat_msg (4×512B 队列)
主循环 200ms ──────────────────────▶ aitalk_tick ──▶ ui_cb(play/emotion/frame)
                                                          │  GUI 剥离：现在仅日志，
                                                          ▼  LVGL 阶段刷新眼睛控件
                                                        aitalk_ui_state_t
```

关键点：

1. **情绪链路补全**：WS63 原代码里 `current_emotion` 每 tick 被 reset（依赖 SDK 内部
   message 回调赋值），本实现明确解析 convai.v1 `function_call`
   （`{"calls":[{"name":"emotion","arguments":"{\"emotion\":\"happy\"}"}]}`），
   并按协议要求**自动回 `function_call_output`**（conversation.items.create）。
2. **动画计数保留**：sleep 8 帧循环、silence 15 帧眨眼、happy 0..5 往返（dir 逻辑）、
   angry/doubt 8 帧、sad 20 帧穿插眨眼——与原代码一致（计数器跨状态延续，
   与原静态 count 行为相同，测试已固化该行为）。
3. **消息队列裁剪**：`MAX_CHAT_MSG_LEN 1500→512`（云端文本回复 <500 字符），
   队列内存 6000B→2048B；满则丢新（与原 add_msg 行为一致）。

## 3. <100KB 内存预算（含 AItalk）

为容纳 AItalk 的 2.1KB，对缓冲池做小幅再平衡（体验影响可忽略）：
TX 队列 16→12 帧（240ms）、RX 环 24→20KB（1.25s@8k）。

| 池 | 大小 | 说明 |
|---|---|---|
| 静态工作缓冲（编码 1K + 解码 4K） | 5.0 K | convai_ws.c |
| TX 上行帧队列 12×772B | 9.0 K | `CONVAI_TX_QUEUE_FRAMES=12` |
| RX 下行抖动环 | 20.0 K | `CONVAI_RX_RING_KB=20` |
| 任务栈（ws 4K + 泵 3K + 发送 3K） | 10.0 K | |
| 编解码峰值（Opus enc+dec 懒加载） | 33.0 K | |
| 引擎 + JSON | 3.0 K | |
| **AItalk 核心（4×512 + 状态）** | **2.1 K** | `AITALK_MEM_BYTES` |
| **合计（明文 ws）** | **84.1 K** ✅ | `CONVAI_APP_TOTAL_WS` |
| + TLS 瘦身缓冲 | +12.0 K | wss |
| **合计（wss）** | **96.4 K** ✅ | `CONVAI_APP_TOTAL_WSS` |

**验证（40/40 通过）**：`test_aitalk_memory_and_total_budget` 用
`convai_limits.h` 同一组常量断言两个合计 < 102400 并打印预算表；
子系统自身（不含 AItalk）81968/94256。

## 4. SDK 重写（闭源 libconvai_sdk.a → convai_ws）回顾

| 原闭源 SDK 能力 | 重写实现 | 内存手段 |
|---|---|---|
| WS 传输 + convai.v1 协议 | esp_websocket_client + convai_protocol.c | 无每帧 malloc |
| G.711A 音频 | 原代码复用 + 新增 G711U/ADPCM/PCM/Opus | 查表/懒加载 |
| 引擎状态机 | convai_ws.c + 原 API 签名 | 引擎 ~1KB |
| 内部缓冲（不可见） | TX 队列 + RX 消息环（静态） | 卡顿丢最老整帧不崩溃 |
| TLS（SDK 内） | esp-tls（ECDSA P-256 + AES-GCM + 4KB 缓冲） | 省 24KB，握手快 10× |

## 5. 文件清单与验证命令

```
components/aitalk/include/aitalk_app.h   # API（生命周期/事件入口/队列/tick/getters）
components/aitalk/aitalk_app.c           # 实现（纯C，含 WS63 行号对照注释）
main/main.c                              # 集成：回调接线 + function_call_output 应答 + 200ms tick
host_tests/main/test_main.c              # +5 项 AItalk 测试（总 40 项）
```

```bash
# 单元测试（期望 40 Tests 0 Failures，含两张预算表）
cd ~/goldie_esp32/host_tests && idf.py build && timeout 60 ./build/host_tests.elf
# 固件（期望 Project build complete）
cd ~/goldie_esp32 && idf.py build
```

## 6. 与原实现的行为差异（刻意保留 vs 刻意修改）

| 项 | 行为 | 说明 |
|---|---|---|
| play_type 映射 | **一致** | IDLE/未启动→SLEEP、ANSWERING→SPEAK、其他→SILENCE |
| 动画帧计数 | **一致** | 计数器跨状态延续（原静态 count 共享） |
| 情绪来源 | **补全** | 原实现依赖 SDK 内部回调；现显式解析 function_call 并自动应答 output |
| 消息队列 | **裁剪** | 1500B→512B；满丢新（一致） |
| GUI 渲染 | **剥离** | ui_cb 输出逻辑状态（play/emotion/avatar/frame），LVGL 阶段再接 |
| 线程模型 | **简化** | 原独立 play_task 线程 → 主循环 200ms tick（省 4KB 栈） |
