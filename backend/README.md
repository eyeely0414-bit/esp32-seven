# 本地语音后端

## 你现在需要做的事

1. 在 VS Code 中把仓库根目录的 `.env.example` 复制为 `.env`。
2. 只在 `.env` 中填写：`DEEPSEEK_API_KEY`、`BAIDU_API_KEY`、`BAIDU_SECRET_KEY`。
3. 不要把 `.env` 内容发送到聊天，也不要提交到 Git。

运行：

```bash
python3 backend/server.py
```

启动后，可在浏览器访问 `http://127.0.0.1:8000/health`，预期返回：

```json
{"status": "ok"}
```

ESP32 下一阶段会把 16 kHz、16 位、单声道 PCM 音频用 `POST /voice` 发送到此服务。服务依次调用百度 ASR、DeepSeek 和百度 TTS，并把 16 kHz PCM 回复音频直接返回给设备播放。

## 当前状态

端到端语音链路已实测通过。启动本服务后，ESP32 长按按键录音、松开提交，即可收到语音回复。

注意：电脑与 ESP32 必须处于同一 Wi-Fi，且电脑切换网络后需更新 `include/secrets.h` 中的 `BACKEND_HOST` 并重新烧录。后端为 Python 在 macOS 上运行时，会使用系统证书库访问百度与 DeepSeek。
