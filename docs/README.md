# Goldie ESP32 研发文档（Spec-Driven Development）

本目录按 Spec-Driven Development（规格驱动开发）方式组织 goldie_esp32 工程的核心设计文档，
目标是让**不了解上下文的开发者（或 AI）仅凭文档即可理解并复现实现**。

## 文档索引

| 文档 | 内容 | 对应 SDD 阶段 |
|---|---|---|
| [01-spec-audio-codec.md](01-spec-audio-codec.md) | 需求规格：编解码架构要做什么、验收标准 | Spec（规格） |
| [02-design-audio-codec.md](02-design-audio-codec.md) | 技术设计：架构、接口、数据结构、协议映射、状态机 | Plan（计划/设计） |
| [03-implementation-guide.md](03-implementation-guide.md) | 实现指南：文件清单、逐步实现、构建与测试命令 | Tasks（任务/实现） |
| [04-performance-and-pitfalls.md](04-performance-and-pitfalls.md) | 性能优化要点与已知坑（含崩溃根因分析） | 横切关注点 |
| [05-memory-optimization-100kb.md](05-memory-optimization-100kb.md) | **100KB 内存优化技术路线**（静态池/懒加载/卡顿弹性/TLS与ECDSA证书）+ WS63 原代码对照 + mock 端云 E2E | 优化专题 |

## 工程概况

- **目标芯片**：ESP32-S3（Xtensa），ESP-IDF v6.2（master，EIM 安装）
- **功能**：用 WebSocket 重新实现 ConvAI 设备端 SDK（convai.v1 协议），
  支持运行时动态切换音频编解码：PCM16 / G.711A / G.711U / IMA-ADPCM / Opus（定点）
- **验证状态**：esp32s3 固件编译通过；主机端（linux target）28/28 单元测试通过；
  各编解码器编码/解码单侧内存均 < 64KB
- **工程位置**：WSL `~/goldie_esp32`（Windows 侧 `\\wsl.localhost\Ubuntu-22.04\home\hubinix\goldie_esp32`）

## 快速验证

```bash
# WSL 内，先激活环境
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/tools/python/master/venv
. $HOME/.espressif/master/esp-idf/export.sh

# 固件构建（esp32s3）
cd ~/goldie_esp32 && idf.py set-target esp32s3 && idf.py build

# 单元测试（linux target，35 项）
cd ~/goldie_esp32/host_tests && idf.py --preview set-target linux && idf.py build
timeout 60 ./build/host_tests.elf

# 端云 E2E（真实设备代码 + 真实网关 + mock 后端）
cd /mnt/d/dev/router && go build -o bin/mockbackends ./cmd/mockbackends && go build -o bin/router ./cmd/router
./bin/mockbackends -asr :51051 -llm :51052 -tts :51061 &
./bin/router -listen :9000 -asr 127.0.0.1:51051 -llm 127.0.0.1:51052 -tts 127.0.0.1:51061 &
cd ~/goldie_esp32/e2e_tests && idf.py --preview set-target linux && idf.py build
timeout 120 ./build/e2e_tests.elf    # 期望 E2E RESULT: PASS
```
