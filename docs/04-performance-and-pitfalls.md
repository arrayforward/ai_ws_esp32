# 04 - 性能优化要点与已知坑（Performance & Pitfalls）

> 横切关注点。本文是"踩坑记录 + 调优手册"，新实现前**先读本文**。

## 一、性能优化要点

### 1. 编解码器选型策略

| 场景 | 推荐 | 理由 |
|---|---|---|
| 局域网网关、带宽充足 | PCM16 / G.711A | 零/极低 CPU，延迟最小 |
| 与电话/传统语音系统互通 | G.711A / G.711U | 行业标准，2:1 |
| 弱 MCU（无 FPU、<100MHz） | IMA-ADPCM | 纯查表整数运算，4:1，单帧 <<1ms |
| 窄带/公网、要音质 | Opus (定点 CELT) | ~10:1，16kbps 语音可懂度好 |

### 2. Opus 定点化（ESP32-S3 必须）

ESP32-S3 的 Xtensa LX7 虽有单精度 FPU，但 Opus 浮点路径（含 SILK float、CELT float）
CPU 开销仍远大于定点。构建宏（缺一不可）：

```
OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API CUSTOM_MODES VAR_ARRAYS HAVE_LRINTF HAVE_LRINT
```

- `FIXED_POINT`：核心，启用整数实现；只编译 `silk/fixed/*.c`，**排除** `silk/float/*`。
- `DISABLE_FLOAT_API`：去掉 `opus_encode_float` 等，省代码空间。
- `VAR_ARRAYS`：用 C99 VLA 做栈上临时缓冲（Xtensa GCC 支持）；
  若工具链不支持 VLA，改用 `USE_ALLOCA`；两者都没有时会退化为非线程安全伪栈（勿用）。
- 运行参数：16kHz / 单声道 / 16kbps / **CBR** / **复杂度 1**。CBR 让每包 40B 固定，
  便于网关按帧长解析；复杂度 1 足够语音可懂度，CPU 减半。

### 3. 懒加载（内存优化核心手段）

Opus 编码器 ≈14.5KB、解码器 ≈18.4KB。语音玩具多数时刻是半双工（说或听），
因此 `init` 不创建任何实例，**首次 encode 才建编码器、首次 decode 才建解码器**。
由此"编码侧 <64KB、解码侧 <64KB"各自成立，而不是两者叠加。

配合 `deinit` 钩子在 `convai_set_codec` / `convai_destroy` 时释放，
切换格式无泄漏（动态切换测试覆盖）。

### 4. 零拷贝与小缓冲

- PCM16 codec 是纯 memcpy，不要在中途再复制。
- 编码输出缓冲 `data_len + 256` 对所有格式恒够：G711 减半、ADPCM 1/4、
  Opus 每 20ms 包 ≤256B（`OPUS_MAX_PACKET` 钳制）。
- 解码缓冲按 `enc_len * 2 + 64` 样本分配，覆盖最坏情况（ADPCM 2 样本/字节）。
- 热路径日志用 `ESP_LOGD`，量产关闭，避免每帧串口开销。

### 5. 构建尺寸

- Opus 增加 ~200KB：app 分区必须 >1MB（本工程 partitions.csv 给 2MB）。
- 不要 Opus 时 `idf.py menuconfig → ConvAI audio codecs` 关闭，固件立省 200KB。

## 二、坑（按踩坑顺序）

### 坑 1：ESP-IDF v6 组件大迁移

**现象**：`Failed to resolve component 'json'`、`esp_websocket_client` 不存在。
**原因**：IDF 6.x 把 cJSON、esp_websocket_client 等迁到组件注册表（managed component）。
**解法**：`main/idf_component.yml` 声明 `espressif/cjson`、`espressif/esp_websocket_client`，
`REQUIRES` 用新名 `cjson`。

### 坑 2：ESP-IDF 组件 CMake 脚本模式限制

**现象**：`add_subdirectory command is not scriptable`、
`CONFIGURE_DEPENDS is invalid for script mode`。
**原因**：IDF 在"获取组件依赖"阶段以 CMake 脚本模式执行每个组件的 CMakeLists，
只允许声明式命令。
**解法**：组件内用普通 `file(GLOB)` + `idf_component_register`，
不要 `add_subdirectory` 第三方 CMake 工程（opus 就是按源文件直接编进来的）。

### 坑 3：linux target 缺 libbsd

**现象**：`fatal error: bsd/sys/cdefs.h`。
**解法**：`sudo apt-get install -y libbsd-dev`。

### 坑 4：IDF 6.2 master 自身在 linux target 的编译错误

**现象**：`nvs_partition_manager.cpp:86: ambiguous overload operator==`。
**解法**：改成 `if (partition == &(*it))`（只影响主机测试构建）。

### 坑 5：默认 1MB 分区放不下 Opus 固件

**现象**：`app partition is too small for binary ... size 0x108670`。
**解法**：自定义 `partitions.csv`（app=2MB）+ `sdkconfig.defaults` 打开
`CONFIG_PARTITION_TABLE_CUSTOM` + flash 4MB，删旧 sdkconfig 重建。

### 坑 6（严重）：vendored opus-1.6.1 的 SILK 编码器堆越界崩溃

**现象**：Opus 编码时随机 `SIGSEGV`，崩在 `opus_custom_encoder_ctl`：
`if (value<1 || value>st->mode->nbEBands)` —— `st->mode == NULL`。
崩溃位置看似 CELT 内部，极具迷惑性。

**gdb 定位过程（可复用的方法论）**：
1. 在 `opus_encode` 入口打印：CELT `mode` 指针有效 → 排除初始化失败；
2. 对 mode 字段下 **硬件 watchpoint**，抓到凶手是一次 `memset(8752 字节, 0)`：
   `silk_Encode`（enc_API.c:190 附近）误触发"mono→stereo 迁移"分支，
   对 `&psEnc->state_Fxx[1]` 调 `silk_init_encoder`——而单声道分配里根本没有
   state_Fxx[1]，memset 正好落在紧随其后的 CELT 编码器头部，把 mode 清零；
3. 第二次进 `silk_Encode` 时 `psEnc->nChannelsInternal` 已是垃圾值，
   说明 silk 编码路径内部先发生了越界写（该定制版 silk 有真实 bug，
   GCC 也报 `maybe-uninitialized` 警告），越界写又触发了错误的迁移分支。

**结论**：这套 vendored opus-1.6.1 的 **SILK 编码器不可信**（goldieos 实际只用
G.711A 上行，Opus 仅"编译备用"，从未被真正跑过，所以 bug 没暴露）。

**解法**：Opus 适配层改用 **`OPUS_APPLICATION_RESTRICTED_LOWDELAY`（纯 CELT）**：

```c
opus_encoder_create(16000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
```

- 完全绕开 SILK 编码路径；CELT 路径经测试稳定（10 帧连续流能量比 0.948）。
- 额外收益：算法延迟最低（CELT 帧可低至 2.5ms），适合实时对话。
- 产出码流是标准 Opus，任何常规 Opus 解码器可解。
- 代价：同码率下语音主观质量略低于 SILK/HYBRID；16kbps 下可接受。
  若未来必须 SILK/HYBRID，应整体替换为上游官方 libopus 并重新验证。

### 坑 7：有损编解码的"往返测试"不能逐样本比

**现象**：Opus encode→decode 后 `|in[i]-dec[i]|` 超差，能量却正常。
**原因**：编解码器有固有时延（look-ahead），输出相对输入整体平移；
且 Opus 有损。逐样本断言必然误报。
**解法**：连续喂多帧（≥10），跳过前 5 帧预热期，用**稳态能量比**
（0.5 < E_dec/E_in < 1.5）+ 帧长断言作为"编解码成功"判据。
G.711/ADPCM 这类无延迟低损格式才可做逐样本容差断言。

### 坑 8：状态释放顺序

`deinit` 必须在 `free(state)` **之前**调用（Opus 内部还有自己的堆块）。
封装层（convai_ws.c 的 set_codec_internal / convai_destroy）和测试代码都遵守此序，
写新测试时注意。

## 三、验证清单（改动后必跑）

```bash
cd ~/goldie_esp32/host_tests && idf.py build && timeout 60 ./build/host_tests.elf
# 期望：28 Tests 0 Failures；[mem] 各行 < 65536

cd ~/goldie_esp32 && idf.py build
# 期望：Project build complete，bin < 0x200000
```
