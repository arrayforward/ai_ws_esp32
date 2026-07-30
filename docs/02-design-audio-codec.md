# 02 - 技术设计：音频编解码架构（Design）

> SDD 阶段：设计（How）。描述架构决策、接口契约、数据流和协议映射。

## 1. 总体架构

```
应用层 (main.c / 未来的 AItalk)
   │  mono PCM16（上行 mic）          │ mono PCM16（下行 TTS）
   ▼                                 ▲
┌─────────────────────────────────────────────┐
│ convai_ws.c  —— 引擎（原 ConvAI SDK 的 API） │
│  convai_create/start/stop/send_audio/...    │
│  convai_set_codec/get_codec                 │
└──────┬───────────────────────┬──────────────┘
       │ 调用统一接口           │ WS 收发
       ▼                       ▼
┌──────────────────┐   ┌────────────────────────┐
│ 编解码注册层      │   │ 协议层 convai_protocol │
│ convai_codec.c   │   │ JSON 信封 + 13B 音频头 │
│ + codec_*.c      │   │ （纯 C，可主机测试）    │
└──────────────────┘   └────────────────────────┘
                               │ esp_websocket_client
                               ▼
                        cloud_gateway (convai.v1)
```

**关键决策**：编解码层与协议层是纯 C（仅依赖 cJSON 和标准库），不碰 FreeRTOS/网络。
引擎层（convai_ws.c）才接触 ESP-IDF。这使全部核心逻辑可在 linux target 上单元测试。

## 2. 编解码器统一接口（`components/convai_ws/include/convai_codec.h`）

```c
typedef enum {
    CONVAI_CODEC_PCM16 = 0, CONVAI_CODEC_G711A = 1, CONVAI_CODEC_G711U = 2,
    CONVAI_CODEC_IMA_ADPCM = 3, CONVAI_CODEC_OPUS = 4, CONVAI_CODEC_MAX
} convai_codec_id_e;              // 编号即线上 hello.audio_codec 值

typedef struct {
    convai_codec_id_e id;
    const char *name;             // "pcm16"/"g711a"/"g711u"/"ima_adpcm"/"opus"
    int  sample_rate;             // 该格式的原生采样率（8k 或 16k）
    size_t state_size;            // 实例状态字节数，0 = 无状态

    int  (*init)(state);                                  // 初始化/复位状态
    int  (*encode)(state, pcm, samples, out, cap, &len);  // PCM16 -> 码流
    int  (*decode)(state, enc, len, pcm, cap, &samples);  // 码流 -> PCM16
    void (*deinit)(state);                                // 释放内部资源（可空）
    size_t (*mem_usage)(state);                           // 当前堆占用（可空=0）
} convai_codec_t;

const convai_codec_t *convai_codec_get(id);        // 按 id 查
const convai_codec_t *convai_codec_by_name(name);  // 按名查（hello_ack 协商用）
size_t convai_codec_count();                       // 编译进固件的个数
const convai_codec_t *convai_codec_at(index);      // 遍历
```

**注册表**（convai_codec.c）就是一个 switch。新增格式的步骤：
写一个 `codec_xxx.c` 实现上述接口 → 在注册表加一行 → 在枚举加 id → 加测试。

## 3. 实例状态管理（内存控制核心）

```
convai_set_codec(id)
  ├─ convai_codec_get(id)           // 未编译 Opus 时返回 NULL -> ERR_NOT_SUPPORTED
  ├─ 旧 codec->deinit(old_state)    // 如 Opus 销毁 OpusEncoder/Decoder
  ├─ free(old_state)
  ├─ calloc(1, c->state_size)       // ADPCM 16B；Opus 仅 2 个指针
  ├─ c->init(new_state)
  └─ e->codec = c
```

- **无状态格式**（PCM/G711A/G711U）：`state_size = 0`，state 传 NULL。
- **IMA-ADPCM**：state = `{predictor, step_index, nibble_buf, have_nibble}` 共 16B，
  必须跨帧保持（ADPCM 是增量编码），编/解各自一份。
- **Opus（懒加载）**：init 只把指针清零；首次 encode 才 `opus_encoder_create`，
  首次 decode 才 `opus_decoder_create`。**编码/解码不同时占内存**，满足 <64KB 预算。
- 引擎销毁（convai_destroy）同样走 deinit→free，防泄漏。

## 4. 数据流

### 4.1 上行（设备 → 网关）

```
mic PCM16 (20ms)
  → convai_send_audio(engine, pcm, len, info)
  → 若 info->data_type == 当前 codec 且 != PCM16：原样透传
    否则：codec->encode(state, pcm, samples, buf, cap, &n)
  → convai_proto_audio_hdr_pack(op=0x10, seq, ts)  // 13B 大端头
  → WS binary frame（分段发送：hdr + payload + fin）
```

### 4.2 下行（网关 → 设备）

```
WS binary frame
  → convai_proto_audio_hdr_unpack → op
  → 0x10 Frame: codec->decode(state, payload) → emit_audio(PCM16)  → 应用播放
  → 0x11 Start: emit_status(ANSWERING)
  → 0x12 End:   emit_status(ANSWER_FINISHED)
```

解码缓冲按最坏情况分配：`enc_len * 2 + 64` 个 int16（ADPCM 每字节 2 样本）。

### 4.3 协商

```
设备 ──TEXT hello {product_*, audio_codec:<当前id>, sample_rate:<codec->sample_rate>}──▶ 网关
设备 ◀─TEXT hello_ack {session_id, audio_config:{codec:"opus"}} ──────────────────────
  └─ 若 audio_config.codec 存在且 != 当前：convai_codec_by_name() → set_codec_internal()
  └─ 随后发送 config_update（人设/TTS 音色，来自 convai_start 的 opt.params）
```

## 5. 各编解码器实现要点

| 格式 | 文件 | 实现要点 |
|---|---|---|
| PCM16 | codec_pcm.c | memcpy 直通；校验长度偶数、容量 |
| G.711A | codec_g711.c + convai_codec_g711a.c | 直接复用 goldieos 的 ITU-T 查表实现 |
| G.711U | codec_g711.c | 标准 μ-law：BIAS=132、CLIP=32635、段码+量化、0x55 反相；静音=0xFF |
| IMA-ADPCM | codec_ima_adpcm.c | Intel/DVI 4bit：89 级步长表 + 16 项索引表；低半字节在前；奇数样本末尾冲刷半字节 |
| Opus | codec_opus.c + components/opus（vendored 1.6.1） | **RESTRICTED_LOWDELAY（纯 CELT）**，16kHz/mono/16kbps/CBR/复杂度1；原因见 04 文档 |

## 6. 协议层设计（convai_protocol.c，纯 C）

- 文本信封：`{"type","seq","ts","body":{...}}`，构建/解析成对（cJSON）。
- 解析规则：`type` 缺失即失败；`body` 非对象则置 NULL（不视为错误）。
- 二进制头 13B：`u8 op | u32 BE seq | u64 BE ts`，pack/unpack 严格大端。
- 状态字符串 ↔ `convai_status_e` 映射集中在此，未知字符串归 IDLE。

## 7. 错误处理

- 所有 API 返回原 SDK 的错误码（`CONVAI_ERR_*`，见 convai_types.h），
  便于上层直接用 `convai_err_2_str()` 打印。
- 发送时 WS 断开 → `CONVAI_ERR_CONNECTION_LOST`；会话未就绪 → `CONVAI_ERR_SESSION_NOT_READY`；
  编解码失败 → `CONVAI_ERR_MEDIA`，该帧丢弃但不影响会话。
- 网关 hello_err → 触发 `CONVAI_EV_FAILED` 事件回调。

## 8. 裁剪与构建开关

| 开关 | 位置 | 作用 |
|---|---|---|
| `CONFIG_CONVAI_ENABLE_OPUS` | components/convai_ws/Kconfig | n 时 Opus 不编译，注册表返回 NULL |
| 分区表 partitions.csv | 工程根 | app 分区扩到 2MB（Opus 使固件超 1MB） |
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | sdkconfig.defaults | 声明 4MB flash |
