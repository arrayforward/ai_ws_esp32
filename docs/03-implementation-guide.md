# 03 - 实现指南（Implementation Guide）

> SDD 阶段：任务（Tasks）。按此文档可从零复现整个实现。每一步都给出
> 确切文件、关键代码决策和验证命令。

## 0. 前置环境

- WSL2 Ubuntu 22.04 + ESP-IDF v6.2（EIM 安装，激活方式见 docs/README.md）
- linux target 测试需要 `sudo apt-get install -y libbsd-dev`（否则缺 `bsd/sys/cdefs.h`）
- ESP-IDF 6.2 的 `json`(cJSON) 与 `esp_websocket_client` 已移出 IDF，
  必须在 `main/idf_component.yml` 声明托管依赖：

```json
{
  "dependencies": {
    "espressif/esp_websocket_client": "^1.5.0",
    "espressif/cjson": "^1.7.18"
  }
}
```
组件 `REQUIRES` 里写 `cjson`（不是旧名字 `json`）。

## 1. 文件清单（按实现顺序）

```
goldie_esp32/
├── CMakeLists.txt                       # include($ENV{IDF_PATH}/tools/cmake/project.cmake)
├── partitions.csv                       # app 分区 2MB（Opus 需要）
├── sdkconfig.defaults                   # 自定义分区表 + 4MB flash
├── main/
│   ├── main.c                           # WiFi STA + ConvAI 生命周期 + 动态切换 demo
│   ├── Kconfig.projbuild                # GOLDIE_WIFI_SSID/PASSWORD/SERVER_URL/产品凭证/AGENT_ID
│   ├── CMakeLists.txt                   # REQUIRES convai_ws esp_wifi esp_event nvs_flash esp_netif cjson
│   └── idf_component.yml                # 托管依赖（见上）
├── components/
│   ├── convai_ws/
│   │   ├── include/
│   │   │   ├── convai_api.h             # 原 SDK 公开 API（convai_create/start/stop/...）
│   │   │   │                            #   + 扩展 convai_set_codec/get_codec
│   │   │   ├── convai_types.h           # 错误码/状态枚举；data_type 枚举与 codec id 一致(0-4)
│   │   │   ├── convai_event.h           # 回调结构 on_event/on_status/on_audio/on_message
│   │   │   ├── convai_protocol.h        # 信封 + 音频头编解码接口
│   │   │   ├── convai_codec.h           # 编解码注册表接口（核心抽象）
│   │   │   └── convai_codec_g711a.h     # goldieos 移植的 G.711A 接口
│   │   ├── convai_ws.c                  # 引擎：WS 事件、消息分发、编解码调用、状态管理
│   │   ├── convai_protocol.c            # 纯 C 协议实现
│   │   ├── convai_codec.c               # 注册表 switch
│   │   ├── codec_pcm.c                  # PCM16 直通
│   │   ├── codec_g711.c                 # G711A 适配 + G711U 实现
│   │   ├── convai_codec_g711a.c         # G.711A 查表实现（来自 goldieos，勿改）
│   │   ├── codec_ima_adpcm.c            # IMA-ADPCM 实现
│   │   ├── codec_opus.c                 # Opus 适配（懒加载、RESTRICTED_LOWDELAY）
│   │   ├── Kconfig                      # CONFIG_CONVAI_ENABLE_OPUS (default y)
│   │   └── CMakeLists.txt               # SRCS 全列；REQUIRES esp_websocket_client cjson esp_timer opus
│   └── opus/
│       └── CMakeLists.txt               # vendored opus-1.6.1 的 ESP-IDF 封装（定点）
├── third_party/opus-1.6.1/              # 从 goldieos 拷贝的 Opus 源码
└── host_tests/                          # linux target 单元测试工程（独立 idf 工程）
    ├── main/test_main.c                 # 28 个 Unity 用例 + app_main 运行器
    ├── main/idf_component.yml           # espressif/cjson
    └── components/convai_testable/
        └── CMakeLists.txt               # 引用 ../../components 下的纯 C 源 + opus 源 + 定点宏
```

## 2. 实现步骤

### Step 1 — 协议层（convai_protocol.c）

纯 C，只依赖 cJSON。要点：

- `convai_proto_build_envelope(type, body_json, seq, ts)`：`body_json==NULL` 时 body 为 `{}`。
- `convai_proto_parse_envelope`：用 `cJSON_ParseWithLength`（数据非 NUL 结尾）；
  body 用 `cJSON_DetachItemFromObjectCaseSensitive` 摘出交给调用方释放。
- 音频头：手写大端移位，**不要**用 `htonl` 之类（嵌入式可移植性）。

验证：host_tests 中 7 个协议用例（往返、空 body、非法 JSON、非对象 body、状态映射、
音频头大小端、短缓冲）。

### Step 2 — 编解码注册层

1. 定义 `convai_codec_t`（见 02 文档 §2）。新字段 `deinit`/`mem_usage` 后加在结构尾部，
   旧格式描述符置 0 即可。
2. 注册表 `convai_codec_get` 用 switch；Opus 分支包在
   `#ifdef CONFIG_CONVAI_ENABLE_OPUS` 中（host 构建未定义时自然裁剪）。

### Step 3 — 各编解码器

- **G.711A**：直接拷贝 goldieos `sdk_integration/convai_codec_g711a.c`，适配层转调。
  静音向量：PCM 0 → 0xD5，0xD5 → 8。
- **G.711U**：标准 μ-law。要点：负数先取反再加 BIAS(132)；CLIP=32635；
  段查找用 `seg_end[8]` 表；静音编码 = 0xFF。
- **IMA-ADPCM**：
  - 两张表：`s_step_table[89]`、`s_index_table[16]`（标准 IMA/DVI）。
  - 编码状态必须跨帧保存（predictor + step_index），半字节缓冲处理奇数样本。
  - 解码每字节产 2 样本（低半字节先）。
- **Opus**：见 02 文档 §5 和 04 文档的坑。**必须**用 `OPUS_APPLICATION_RESTRICTED_LOWDELAY`。

### Step 4 — 引擎（convai_ws.c）

- WS 客户端：`esp_websocket_client_init({.uri, .subprotocol="convai.v1"})`，
  `esp_websocket_register_events(... WEBSOCKET_EVENT_ANY ...)`。
- 事件分发：`WEBSOCKET_EVENT_DATA` 按 `op_code` 0x1=文本 / 0x2=二进制。
- 文本消息处理顺序：`hello_ack`（置 session_ready + 可选自动切 codec + 发 config_update）、
  `hello_err`、`status`、`event`、`pong`（忽略）、其余原样抛给 on_message。
- 音频发送用三段式：`send_bin_partial(hdr)` → `send_cont_msg(payload)` → `send_fin()`。
- 编码缓冲：`malloc(data_len + 256)` 对所有格式都够（都只会变小或 Opus 小包）。

### Step 5 — Opus 组件封装（components/opus/CMakeLists.txt）

**注意**：ESP-IDF 组件 CMakeLists 在依赖扫描阶段以"脚本模式"执行，
**不能**用 `add_subdirectory`（报 "not scriptable"），也不能用
`file(GLOB ... CONFIGURE_DEPENDS)`。用普通 GLOB + 显式排除：

```cmake
set(OPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/opus-1.6.1)
file(GLOB OPUS_SRC ${OPUS_DIR}/src/*.c)
file(GLOB OPUS_CELT ${OPUS_DIR}/celt/*.c)
file(GLOB OPUS_SILK ${OPUS_DIR}/silk/*.c)
file(GLOB OPUS_SILK_FIX ${OPUS_DIR}/silk/fixed/*.c)
list(REMOVE_ITEM OPUS_SRC
    ${OPUS_DIR}/src/analysis.c ${OPUS_DIR}/src/mlp.c ${OPUS_DIR}/src/mlp_data.c
    ${OPUS_DIR}/src/opus_projection_encoder.c ${OPUS_DIR}/src/opus_projection_decoder.c
    ${OPUS_DIR}/src/mapping_matrix.c)          # 浮点专用 + 环绕声，定点单声道不需要
idf_component_register(
    SRCS ${OPUS_SRC} ${OPUS_CELT} ${OPUS_SILK} ${OPUS_SILK_FIX}
    INCLUDE_DIRS ${OPUS_DIR}/include
    PRIV_INCLUDE_DIRS ${OPUS_DIR}/src ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API CUSTOM_MODES VAR_ARRAYS HAVE_LRINTF HAVE_LRINT)
```

定点模式只编译 `silk/fixed/*.c`（**不要** `silk/float/`）。

### Step 6 — 测试工程（host_tests/）

- 独立 idf 工程，`idf.py --preview set-target linux`。
- `convai_testable` 组件用相对路径 `../../../components/convai_ws/*.c` 引用被测源码，
  Opus 源同样 GLOB 引入，并 `target_compile_definitions(... PUBLIC CONFIG_CONVAI_ENABLE_OPUS=1 ...)`
  使注册表打开 Opus 分支。
- host 端 GCC 11 会对 silk 报 `-Werror=maybe-uninitialized`（误报），需：
  `target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized)`。
- 运行器用朴素 `UNITY_BEGIN/RUN_TEST/UNITY_END`，跑完进程不退出属正常（用 `timeout` 包一下）。

### Step 7 — 全量验证

```bash
# 1) 主机测试（应 28/28 PASS）
cd ~/goldie_esp32/host_tests && idf.py build && timeout 60 ./build/host_tests.elf

# 2) 固件（应 Project build complete，固件 ~1.05MB < 2MB 分区）
cd ~/goldie_esp32 && idf.py set-target esp32s3 && idf.py build
```

## 3. 内存测试的写法（可复用模式）

```c
/* 每个 codec 两个全新状态分别测编码侧/解码侧 */
void *st = calloc(1, c->state_size);
c->init(st);
c->encode(st, tone, frame, enc, cap, &n);
size_t enc_mem = c->mem_usage ? c->mem_usage(st) : 0;
TEST_ASSERT_LESS_THAN_UINT32(64 * 1024, enc_mem);
if (c->deinit) c->deinit(st);   // 先 deinit 再 free，顺序不能反！
free(st);
```

Opus 的 `mem_usage` 用官方 API 精确计算：
`opus_encoder_get_size(1)` / `opus_decoder_get_size(1)`（懒加载，未创建的一侧计 0）。

## 4. 已知必须的 ESP-IDF 6.2 修补

`components/nvs_flash/src/nvs_partition_manager.cpp:86` 在 linux target + GCC11 下编译失败
（IDF master 自身 bug，iterator 与指针比较）：

```cpp
// 原代码（编译错误）            // 修复为
if (partition == it) {           if (partition == &(*it)) {
```

该修补只影响 linux target 主机测试构建，不影响固件。
