# 06 - 从 WS63 到 ESP32：端侧 SDK 迁移与适配执行手册

> 目标读者：需要**照着执行编码**的开发者或 AI。
> 每个阶段都给出：任务清单、要创建/修改的具体文件、关键代码骨架、
> 验证命令和通过标准（Gate）。未通过 Gate 不要进入下一阶段。
>
> 配套阅读：docs/01（规格）、02（设计）、03（实现指南）、04（坑）、05（内存路线）。

## 0. 参考文档与下载链接

### 0.1 ESP-IDF 官方文档（部署依据）

| 文档 | 链接 |
|---|---|
| ESP-IDF 编程指南（ESP32-S3，中文） | https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/ |
| **Linux 下安装 ESP-IDF 及工具链**（本部署的原始依据） | https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/linux-setup.html |
| Windows 下安装（对照参考） | https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/windows-setup.html |
| EIM 安装管理器下载页（GUI/CLI 安装包） | https://dl.espressif.com/dl/eim/ |
| EIM 使用文档 | https://docs.espressif.com/projects/idf-im-ui/en/latest/ |
| ESP-IDF 版本说明（选稳定版用） | https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32/versions.html |
| ESP32-S3 技术参考手册（外设/寄存器） | https://www.espressif.com.cn/sites/default/files/documentation/esp32-s3_technical_reference_manual_cn.pdf |
| ESP32-S3 数据手册（电气/封装） | https://www.espressif.com.cn/sites/default/files/documentation/esp32-s3_datasheet_cn.pdf |

### 0.2 组件与第三方库

| 组件 | 链接 |
|---|---|
| esp_websocket_client（组件注册表） | https://components.espressif.com/components/espressif/esp_websocket_client |
| cJSON（组件注册表） | https://components.espressif.com/components/espressif/cjson |
| ESP-SR（唤醒词/AFE 语音前端，阶段 7 用） | https://github.com/espressif/esp-sr |
| LVGL（GUI 替代 tiny_gui，阶段 7 用） | https://github.com/lvgl/lvgl |
| Opus 官方（算法与 API 参考） | https://opus-codec.org/ 、 https://opus-codec.org/docs/html_api/ |
| vendored opus-1.6.1 源码 | `D:\ai-hardware-agent-examples\examples\goldieos\third_party\opus-1.6.1`（本仓库 third_party 已内置） |

### 0.3 项目相关仓库与协议

| 内容 | 位置 |
|---|---|
| convai.v1 线协议（**迁移的协议依据**） | `D:\ai-hardware-agent-examples\cloud_gateway\docs\cloud_gateway\protocol.md` |
| ConvAI SDK 公开头文件（API 兼容依据） | `D:\ai-hardware-agent-examples\include\convai\`（convai_api/types/event.h） |
| 原 WS63 固件（移植素材库） | `D:\ai-hardware-agent-examples\examples\goldieos\` |
| 本工程（ESP32 端侧） | https://github.com/arrayforward/ai_ws_esp32 |
| 配套 Go 网关（含 mock 后端） | https://github.com/arrayforward/go-esp32-ws-server |

### 0.4 网关/工具链环境

| 工具 | 链接 |
|---|---|
| Go 1.22 下载（国内镜像） | https://golang.google.cn/dl/go1.22.12.linux-amd64.tar.gz （官站 https://go.dev/dl/ ） |
| Go 模块代理（国内必配） | https://goproxy.cn |
| gRPC Go 快速开始 | https://grpc.io/docs/languages/go/quickstart/ |
| usbipd-win（WSL 透传串口烧录） | https://learn.microsoft.com/zh-cn/windows/wsl/connect-usb 、 https://github.com/dorssel/usbipd-win/releases |
| WSL 安装指南 | https://learn.microsoft.com/zh-cn/windows/wsl/install |

## 1. 背景与迁移决策（必读，决定后面所有取舍）

| 事实 | 结论 |
|---|---|
| ConvAI 云端 SDK 是 `libconvai_sdk.a`（RISC-V/musl 预编译，无源码） | 无法链接到 Xtensa → 必须重写 |
| 线协议 convai.v1 有官方文档 `cloud_gateway/docs/cloud_gateway/protocol.md` | 重写**有规格可依**，不是逆向工程 |
| 应用层（AItalk 等）只调用 `convai_*` 公开 API + 4 个回调 | 重写时**保持 API 签名不变**，上层零改动 |
| goldieos 的 G.711A、opus-1.6.1、配置 JSON 结构是源码 | 直接复用，不重写 |

### 1.1 迁移总原则

1. 协议线上格式逐字节兼容（云端/网关零改动）；
2. `convai_*` API 与回调签名与原 SDK 头文件一致（`include/convai/*.h` 原样拷贝）；
3. 每一层都是"先纯 C 可测，再接平台 API"；每层有 Gate 验证。

## 2. 总体架构（目标态）

```
应用层（AItalk 等，阶段 6 平移）
  │  convai_create/start/stop/update/send_audio/send_message + 4 回调
  ▼
components/convai_ws/                  ← 替代 libconvai_sdk.a + convai_bridge.c
  ├ convai_ws.c        引擎（WS 事件、协商、任务编排、缓冲调度）
  ├ convai_protocol.c  信封 + 13B 音频头（纯 C）
  ├ convai_ring.c      消息环形缓冲（纯 C）
  ├ convai_codec.c     编解码注册表（纯 C）
  └ codec_*.c          PCM16/G711A/G711U/IMA-ADPCM/Opus（纯 C，Opus 经 cgo 无、ESP 用 vendored 源码）
平台层：esp_websocket_client / esp-tls / FreeRTOS / cJSON(托管)
第三方：third_party/opus-1.6.1（定点 CELT）
```

## 3. 阶段 0：开发环境部署（完整操作过程）

> 依据官方文档
> [《在 Linux 上安装 ESP-IDF 及工具链》](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/linux-setup.html)
> 在 **WSL2 Ubuntu 22.04** 中执行（WSL 与原生 Ubuntu 步骤相同）。
> 以下为实测通过的完整命令序列，可直接照抄执行。

### 3.1 准备 WSL2（Windows 侧，已完成可跳过）

```powershell
wsl --install                      # 或 wsl --install -d Ubuntu-22.04
wsl --list --verbose               # 确认 Ubuntu-22.04 为 WSL2（VERSION 2）
```

### 3.2 安装 EIM（ESP-IDF 安装管理器）与系统依赖

```bash
# 1) 添加乐鑫 APT 源（对应官方文档"第二步：通过 APT 安装 EIM"）
echo "deb [trusted=yes] https://dl.espressif.com/dl/eim/apt/ stable main" | \
  sudo tee /etc/apt/sources.list.d/espressif.list
sudo apt update

# 2) 安装 EIM CLI（对应文档"仅 CLI：sudo apt install eim-cli"）
sudo apt install -y eim-cli

# 3) 安装编译依赖（对应文档"第一步：安装依赖包" + 本工程额外需要）
sudo apt install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 \
  libbsd-dev protobuf-compiler libopus-dev pkg-config
#    └ libbsd-dev：linux target 主机测试必需（缺则报 bsd/sys/cdefs.h）
#    └ libopus-dev：Go 网关侧 Opus（cgo）必需
```

### 3.3 用 EIM 安装 ESP-IDF（对应官方文档"第三步"）

```bash
eim install                    # 非交互默认安装（等价于文档"简易安装"）
# 如需指定稳定版（推荐生产用）：
# eim install -i v5.4.2

# 激活环境（每个新终端都要做；写入 ~/.bashrc 一劳永逸）
echo "alias get_idf='. \$HOME/.espressif/tools/activate_idf_master.sh'" >> ~/.bashrc
get_idf
# 脚本化构建（CI/自动化）改用：
#   export IDF_PYTHON_ENV_PATH=$HOME/.espressif/tools/python/master/venv
#   . $HOME/.espressif/master/esp-idf/export.sh
# 注意：EIM 的 activate 脚本不能在子 shell 里 source（会误判非交互），
#       且需把 espidf.constraints.vX.Y.txt 从 tools/ 复制到 ~/.espressif/ 根目录：
cp ~/.espressif/tools/espidf.constraints.*.txt ~/.espressif/ 2>/dev/null || true
```

安装完成后的目录布局（后续所有路径的基准）：

```
~/.espressif/master/esp-idf/     # IDF 源码（$IDF_PATH）
~/.espressif/tools/              # 工具链（xtensa-esp-elf-gcc、cmake、ninja、openocd）
~/.espressif/tools/python/master/venv/   # Python 虚拟环境（idf.py 使用）
```

### 3.4 部署验证（Gate：hello_world 编译通过）

```bash
get_idf
cp -r $IDF_PATH/examples/get-started/hello_world ~/
cd ~/hello_world
idf.py set-target esp32s3
idf.py build
# 通过标准：Project build complete，生成 build/hello_world.bin
```

### 3.5 Go 网关侧环境（E2E/联调需要）

```bash
# Go 1.22（官站不可达时用国内镜像）
curl -sL -o /tmp/go.tgz https://golang.google.cn/dl/go1.22.12.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf /tmp/go.tgz
export PATH=$PATH:/usr/local/go/bin:~/go/bin
echo 'export GOPROXY=https://goproxy.cn,direct' >> ~/.bashrc   # 国内必须

# gRPC 代码生成插件
go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.34.2
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.5.1

# 网关构建验证
git clone git@github.com:arrayforward/go-esp32-ws-server.git /mnt/d/dev/router
cd /mnt/d/dev/router && go build ./... && go test ./internal/codec/
```

### 3.6 串口透传（上板烧录，可选）

```powershell
# Windows 侧安装 usbipd-win 后（管理员 PowerShell）：
usbipd list                        # 找到 ESP32-S3 串口 BUSID
usbipd bind --busid 3-2
usbipd attach --wsl --busid 3-2
```

```bash
# WSL 内确认 /dev/ttyUSB0 或 /dev/ttyACM0 出现后即可：
idf.py -p /dev/ttyACM0 flash monitor
```

### 3.7 部署完成度自检表

| 检查项 | 命令 | 期望 |
|---|---|---|
| IDF 环境 | `get_idf && idf.py --version` | 6.x |
| 固件构建 | `cd ~/hello_world && idf.py build` | build complete |
| 主机测试 | `sudo apt install libbsd-dev` 后构建 host_tests | 可编译 linux target |
| Go | `go version` | ≥1.22 |
| 网关 | `cd /mnt/d/dev/router && go build ./...` | 无错 |
| 串口（可选） | `ls /dev/ttyACM* /dev/ttyUSB*` | 有设备节点 |

### 3.8 从零重建工程（拿到仓库后）

```bash
get_idf
git clone git@github.com:arrayforward/ai_ws_esp32.git ~/goldie_esp32
cd ~/goldie_esp32
idf.py set-target esp32s3 && idf.py build          # 固件
cd host_tests && idf.py --preview set-target linux && idf.py build \
  && timeout 60 ./build/host_tests.elf             # 35 项单元测试
```

## 4. 阶段 1：协议规格化（Gate：协议层 7 项测试过）

### 3.1 任务

1. 精读 `cloud_gateway/docs/cloud_gateway/protocol.md`（convai.v1 全文）。
2. 兜底验证（可选）：`strings libconvai_sdk.a | grep -iE 'wss?://|session|g711|input_audio'` 确认协议轮廓一致。
3. 建工程骨架并实现协议层。

### 3.2 文件与代码

```
goldie_esp32/
├── CMakeLists.txt            # include($ENV{IDF_PATH}/tools/cmake/project.cmake) + project()
├── components/convai_ws/
│   ├── include/convai_api.h / convai_types.h / convai_event.h   # 从 WS63 仓库 include/convai/ 原样拷贝
│   ├── include/convai_protocol.h                                # 新建：信封+音频头接口
│   └── convai_protocol.c                                        # 新建：实现（纯C，仅依赖 cJSON）
└── main/idf_component.yml    # {"dependencies":{"espressif/cjson":"^1.7.18"}}
```

协议层必须实现的 6 个函数（签名照抄自 `include/convai_protocol.h`）：

```c
char *convai_proto_build_envelope(const char *type, const char *body_json, uint32_t seq, uint64_t ts);
int   convai_proto_parse_envelope(const char *data, size_t len, convai_envelope_t *out);
void  convai_proto_envelope_free(convai_envelope_t *env);
convai_status_e convai_proto_status_from_str(const char *s);
void  convai_proto_audio_hdr_pack(uint8_t hdr[13], uint8_t op, uint32_t seq, uint64_t ts);
int   convai_proto_audio_hdr_unpack(const uint8_t *data, size_t len, uint8_t *op, uint32_t *seq, uint64_t *ts);
```

实现要点：
- 信封字段固定为 `{"type","seq","ts","body"}`；body 用 `cJSON_DetachItemFromObjectCaseSensitive` 交出所有权。
- 音频头**手写大端移位**（可移植，别用 htonl）：`[0]=op, [1:5]=seq BE, [5:13]=ts BE`。
- 解析用 `cJSON_ParseWithLength`（WS 数据不带 NUL）。

### 3.3 Gate

建 `host_tests/`（linux target 测试工程，见阶段 2.2 结构），跑 7 个协议用例：
信封往返、空 body 为 `{}`、非法 JSON 返回 -1、非对象 body 置 NULL、6 个状态字符串映射、
音频头大端往返（0xDEADBEEF/0x0102030405060708）、短缓冲返回 -1。**全过才继续**。

**坑**：IDF 6.x 的 cJSON 是托管组件 `espressif/cjson`，`REQUIRES cjson`（不是 `json`）。

## 5. 阶段 2：编解码层（Gate：21 项测试过）

### 4.1 任务与文件

统一接口（`include/convai_codec.h`）：`init/encode/decode/deinit/mem_usage + state_size + name + sample_rate`，
注册表 `convai_codec_get/by_name/at/count`。id 即线上 `hello.audio_codec` 值：

| id | 文件 | 来源与实现 |
|---|---|---|
| 0 PCM16 | codec_pcm.c | 新写，memcpy 直通 |
| 1 G711A | codec_g711.c + convai_codec_g711a.c | **goldieos `sdk_integration/convai_codec_g711a.c` 原样拷贝** + 适配层 |
| 2 G711U | codec_g711.c | 新写：BIAS=132、CLIP=32635、seg_end 表、异或 0x55；静音=0xFF |
| 3 IMA-ADPCM | codec_ima_adpcm.c | 新写：89 级步长表+16 项索引表；状态(predictor,step_index)跨帧；低半字节在前 |
| 4 Opus | codec_opus.c + components/opus/ | goldieos `third_party/opus-1.6.1` 整树拷贝，定点构建 |

### 4.2 Opus 构建（最容易踩坑，照抄）

1. `cp -r goldieos/third_party/opus-1.6.1 goldie_esp32/third_party/`
2. `components/opus/CMakeLists.txt`：GLOB `src/*.c celt/*.c silk/*.c silk/fixed/*.c`，
   **排除** `analysis.c mlp.c mlp_data.c opus_projection_*.c mapping_matrix.c`（浮点/环绕声）；
   宏：`OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API CUSTOM_MODES VAR_ARRAYS HAVE_LRINTF HAVE_LRINT`。
   **不能**用 `add_subdirectory`（IDF 组件脚本模式限制）。
3. `codec_opus.c`：**必须 `OPUS_APPLICATION_RESTRICTED_LOWDELAY`**（纯 CELT）。
   禁止 VOIP/HYBRID——vendored 树的 SILK 编码器有堆越界 bug 会崩
   （根因与 gdb 定位过程见 docs/04 坑 6）。
   懒加载：init 只清零指针，首次 encode/decode 才 create。
4. 分区表：`partitions.csv` app=2MB + `sdkconfig.defaults` 打开自定义分区 + 4MB flash。

### 4.3 Gate（21 项测试）

注册表 2 项 + PCM 1 + G711A 6（静音 0xD5/0xD5→8/往返保号限差/参数错/立体声 planar）+
G711U 2（静音 0xFF/往返）+ ADPCM 2（4:1 比/正弦往返收敛）+ Opus 2（10 帧流能量比 0.5~1.5、包 ≤256B）+
内存 5 项（每格式编码侧/解码侧 <64KB，Opus 用 `opus_encoder_get_size(1)` 记账）+ 动态切换 1 项。

**测试准则**：有损格式（Opus）禁止逐样本断言——编解码器有时延，必须喂 ≥10 帧、
跳 5 帧预热、比稳态能量。G711/ADPCM 可做逐样本容差断言。

## 6. 阶段 3：引擎与传输（Gate：固件编译过 + mock E2E 过）

### 5.1 任务

`convai_ws.c` 实现公开 API（内部结构 `convai_engine_s`）：

| API | 行为 |
|---|---|
| convai_create | 解析 config_json（info 四凭证 + ws.url + ws.audio.codec），set_codec_internal 初始化 |
| convai_start | 建 TX 队列/RX 环/两任务 → esp_websocket_client_init(uri, subprotocol="convai.v1", task_stack=4096) → start |
| WS 事件 CONNECTED | 发 hello（product_* + audio_codec=codec->id + sample_rate） |
| 收 hello_ack | 置 session_ready → 事件 CONNECTED + 状态 LISTENING → 发 config_update（opt.params 人设） |
| 收 status/event | 映射到 on_convai_conversation_status / on_convai_event |
| 收二进制 0x10 | decode → RX 环 → 泵任务 emit_audio(PCM16)；0x11/0x12 → ANSWERING/ANSWER_FINISHED |
| convai_send_audio | 输入 PCM16 → 按 `codec->sample_rate/50` 切片 → encode → TX 队列 → 发送任务发 13B 头+负载 |
| convai_set_codec | 切换 + 清空在途队列/环 |
| convai_stop/destroy | bye → 停 WS → 停任务 → deinit codec → free |

### 5.2 静态内存三件套（直接照抄数值）

```c
static uint8_t  s_enc_buf[1024];        // 20ms 最大编码输出 640B
static int16_t  s_dec_pcm[2048];        // 解码样本封顶
static uint8_t  s_tx_storage[16 * 772]; // TX 队列（xQueueCreateStatic）
static uint8_t  s_rx_arena[24 * 1024];  // RX 消息环
```

丢帧规则：TX 满→丢最老帧；RX 满→丢最老**整帧**（convai_ring 按 4B 头+负载为消息单位）。
**绝不阻塞、绝不 malloc**——这是卡顿时不崩溃的关键。

### 5.3 Gate

1. `idf.py set-target esp32s3 && idf.py build` 通过；
2. mock E2E（见下节）g711a + opus 两场景 PASS。

**坑**：`xQueueSpacesAvailable` 正确名字是 `uxQueueSpacesAvailable`；
FreeRTOS 句柄删除前要 `tasks_running=false` 并给信号量唤醒任务再删。

## 7. 阶段 4：内存硬化 <100KB（Gate：预算断言过）

单一事实源 `include/convai_limits.h`：所有静态池尺寸 + 估算常量 + 预算宏：

```c
CONVAI_STATIC_POOL_BYTES = enc1K + dec4K + tx16x772 + rx24K   // ≈ 42 KB
CONVAI_SUBTOTAL_WS  = 静态池 + 任务栈10K + codec峰值33K + json/引擎3K   // ≈ 89 KB
CONVAI_SUBTOTAL_WSS = + TLS瘦身12K                                       // ≈ 101 KB
```

测试 `test_subsystem_budget_under_100kb` 用同一组常量断言两个合计 < 102400 并打印预算表。
Kconfig 暴露弹性：`CONVAI_TX_QUEUE_FRAMES / CONVAI_RX_RING_KB / CONVAI_WS_TASK_STACK`。
TLS 瘦身：`sdkconfig.defaults` 加 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096 / OUT=4096`。

## 8. 阶段 5：wss 与证书（Gate：wss 预算断言过）

1. `convai_start` 检测 `wss://` 前缀：
   - 存在 `components/convai_ws/certs/server_ca.pem`（自签 CA）→ CMake 自动嵌入，`cfg.cert_pem`；
   - 否则 `cfg.crt_bundle_attach = esp_crt_bundle_attach`（公共 CA）；
   - Kconfig `CONVAI_WSS_SKIP_CN_CHECK`（IP+自签）。
2. 证书选型（端侧性能，详见 docs/05 §1.4）：
   **ECDSA P-256**（非 RSA-2048，握手快 10 倍）、**AES-256/128-GCM 套件**（S3 硬件 AES）、
   TLS 1.2 + ECDHE。网关仓库 `scripts/gen_cert.sh` 生成配套证书。
3. REQUIRES 加 `esp-tls`。

## 9. 阶段 6：联调与验证（Gate：35 项测试 + E2E PASS）

### 8.1 三层验证体系（务必全建）

| 层 | 工程 | 内容 | 命令 |
|---|---|---|---|
| 单元 | host_tests/（linux target） | 35 项：协议 7 + 编解码 14 + 内存 7 + 环 5 + 预算 2 | `idf.py --preview set-target linux && idf.py build && timeout 60 ./build/host_tests.elf` |
| E2E | e2e_tests/（linux target） | 真实 convai_ws ↔ 真实网关 ↔ mock 后端 | 见 8.2 |
| 上板 | esp32s3 固件 | 编译 + flash monitor | `idf.py build flash monitor`（usbipd 透传串口） |

### 8.2 mock E2E 搭建（go-esp32-ws-server 仓库）

```bash
# 网关仓库已有 cmd/mockbackends（canned ASR 500ms出句/LLM固定回复/TTS 0.8s正弦）
./bin/mockbackends -asr :51051 -llm :51052 -tts :51061 &
./bin/router -listen :9000 -asr 127.0.0.1:51051 -llm 127.0.0.1:51052 -tts 127.0.0.1:51061 &

# 端侧 e2e（goldie_esp32/e2e_tests，linux target 链接真实 convai_ws）
cd ~/goldie_esp32/e2e_tests && idf.py --preview set-target linux && idf.py build
timeout 120 ./build/e2e_tests.elf   # 期望 E2E RESULT: PASS
```

E2E 断言点（e2e_main.c 照此写）：CONNECTED 事件 → LISTENING → 发 30 帧上行 →
收到 text 回复 → TTS Start→帧(≥5)→End → thinking/answering/answer_finished 状态齐全。

**坑**：本机真实 asr_server/tts_server 占用 50051/50061——mock 用 51051/51052/51061 避开；
esp_websocket_client 在 linux target 有个 `-Werror=overflow` 警告，工程根 CMakeLists 加：
`target_compile_options(${ws_lib} PRIVATE -Wno-error=overflow)`（`idf_component_get_property` 取组件库）。

## 10. 阶段 7：未迁移模块路线图（按优先级执行）

| 顺序 | 模块 | 具体做法 | 参考（WS63 源码） |
|---|---|---|---|
| P0 | ES8311 音频 | 用 ESP-IDF 组件 `espressif/esp_codec_dev` 或平移寄存器表；I2S 8k/16k mono；录音线程喂 convai_send_audio，on_audio 回调写 I2S 播放 | `drivers/codec/es8311_drv/es8311.c` |
| P0 | 上板联调 | menuconfig 配 GOLDIE_WIFI_SSID/PASSWORD + GOLDIE_SERVER_URL；usbipd 透传串口烧录 | — |
| P1 | AItalk 状态机 | 平移 467 行逻辑：回调签名已兼容，把 goldie 消息队列换 FreeRTOS Queue | `apps/AItalk/main_app.cpp` |
| P1 | 唤醒词 | ESP-SR WakeNet（模型需重训"你好小荷"）或保留外部 ASR 芯片 GPIO 触发 convai_bridge_start | `services/aud_algo/aualgo_service.c` |
| P2 | GUI | tiny_gui(闭源)→LVGL；RGB565 位图资源、806KB 中文字库可数据级复用 | `apps/*/assets/`、`include/gui/ws63_font.h` |
| P2 | 闹钟/NTP/电源 | alarm_service 平移；ntp→esp_sntp；aw9523b/pcf8563/bat_driver 按新板重写 I2C 层 | `services/`、`drivers/` |
| P3 | 对讲机 | 星闪→ESP-NOW/BLE 重写，或裁剪 | `services/sle_service/` |

## 11. 经验清单（压缩版，执行时贴墙）

1. 先找协议文档；没有才 `strings libxxx.a | grep -E 'wss?://|session|json'` 逆向兜底。
2. 纯 C 先行：协议/编解码/环先在 linux target 测过，再接任何 ESP-IDF API。
3. 第三方库"编译过 ≠ 能跑"：每条路径都要真实数据"编码→解码"验证。
4. API 签名与原 SDK 保持逐字一致——上层应用零改动平移。
5. 内存三件套：静态定界 + 丢帧计数 + 水位观测（convai_mem_report）。
6. 有损编解码测试用多帧稳态能量比，禁止逐样本比。
7. E2E 不等硬件：linux target 能跑真网络栈，把上板问题压缩到只剩驱动。
