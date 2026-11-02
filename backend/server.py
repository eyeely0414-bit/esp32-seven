"""Local voice bridge: ESP32 PCM -> ASR -> DeepSeek -> TTS -> ESP32 PCM.

Run from the repository root with:
    python3 backend/server.py
"""

from __future__ import annotations

import base64
import json
import os
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
ENV_PATH = ROOT / ".env"
TOKEN_URL = "https://aip.baidubce.com/oauth/2.0/token"
ASR_URL = "https://vop.baidu.com/server_api"
TTS_URL = "https://tsn.baidu.com/text2audio"
DEEPSEEK_URL = "https://api.deepseek.com/chat/completions"
SAMPLE_RATE = 16000
# Python installed from python.org on macOS does not always inherit the system
# certificate store. Use macOS's bundled CA file for the two HTTPS providers.
SSL_CONTEXT = ssl.create_default_context(cafile="/etc/ssl/cert.pem")


def load_env(path: Path) -> None:
    """Load simple KEY=VALUE lines without logging secrets."""
    if not path.exists():
        return
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip())


load_env(ENV_PATH)


class ServiceError(RuntimeError):
    pass


class BaiduToken:
    value = ""
    expires_at = 0.0

    @classmethod
    def get(cls) -> str:
        if cls.value and time.time() < cls.expires_at:
            return cls.value

        api_key = required_env("BAIDU_API_KEY")
        secret_key = required_env("BAIDU_SECRET_KEY")
        query = urllib.parse.urlencode(
            {
                "grant_type": "client_credentials",
                "client_id": api_key,
                "client_secret": secret_key,
            }
        )
        payload = request_json(f"{TOKEN_URL}?{query}", method="POST")
        token = payload.get("access_token")
        if not token:
            raise ServiceError(f"Baidu token error: {payload.get('error_description', payload)}")
        cls.value = token
        cls.expires_at = time.time() + max(60, int(payload.get("expires_in", 0)) - 120)
        return token


def required_env(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise ServiceError(f"Missing {name}; copy .env.example to .env and fill it locally.")
    return value


def request_json(
    url: str, *, method: str = "POST", body: bytes | None = None, headers: dict[str, str] | None = None
) -> dict[str, Any]:
    request = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=35, context=SSL_CONTEXT) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise ServiceError(f"Upstream HTTP {error.code}: {detail[:300]}") from error
    except urllib.error.URLError as error:
        raise ServiceError(f"Upstream connection failed: {error.reason}") from error


def transcribe(pcm: bytes, device_id: str) -> str:
    if not pcm:
        raise ServiceError("Received an empty recording.")
    payload = {
        "format": "pcm",
        "rate": SAMPLE_RATE,
        "channel": 1,
        "cuid": device_id[:60],
        "token": BaiduToken.get(),
        "dev_pid": 1537,
        "len": len(pcm),
        "speech": base64.b64encode(pcm).decode("ascii"),
    }
    result = request_json(
        ASR_URL,
        body=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    if result.get("err_no") != 0 or not result.get("result"):
        raise ServiceError(f"ASR failed: {result.get('err_msg', result)}")
    return str(result["result"][0]).strip()


def generate_reply(transcript: str) -> str:
    body = {
        "model": os.getenv("DEEPSEEK_MODEL", "deepseek-flash"),
        "thinking": {"type": "disabled"},
        "max_tokens": 100,
        "messages": [
            {
                "role": "system",
                "content": (
                    "你是脑梗康复训练的温和陪伴助手。回答只用简短、清晰的中文，"
                    "不超过45个汉字。你可以鼓励、提醒按医嘱训练或建议联系家人/康复师；"
                    "不要做诊断、开处方、保证康复结果。若用户描述紧急危险症状，"
                    "明确建议立刻联系急救服务或医生。"
                ),
            },
            {"role": "user", "content": transcript},
        ],
    }
    result = request_json(
        DEEPSEEK_URL,
        body=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {required_env('DEEPSEEK_API_KEY')}",
        },
    )
    try:
        reply = result["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, AttributeError) as error:
        raise ServiceError(f"DeepSeek response was invalid: {result}") from error
    if not reply:
        raise ServiceError("DeepSeek returned an empty reply.")
    return reply


def synthesize(text: str, device_id: str) -> bytes:
    form = urllib.parse.urlencode(
        {
            "tex": text,
            "tok": BaiduToken.get(),
            "cuid": device_id[:60],
            "ctp": 1,
            "lan": "zh",
            "spd": 5,
            "pit": 5,
            "vol": 8,
            "per": 0,
            "aue": 4,  # 16 kHz, 16-bit mono PCM
            "audio_ctrl": json.dumps({"sampling_rate": SAMPLE_RATE}),
        }
    ).encode("utf-8")
    request = urllib.request.Request(
        TTS_URL,
        data=form,
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(request, timeout=35, context=SSL_CONTEXT) as response:
            data = response.read()
            content_type = response.headers.get("Content-Type", "")
    except urllib.error.URLError as error:
        raise ServiceError(f"TTS connection failed: {error.reason}") from error
    if not content_type.lower().startswith("audio"):
        raise ServiceError(f"TTS failed: {data.decode('utf-8', errors='replace')[:300]}")
    return data


class VoiceHandler(BaseHTTPRequestHandler):
    server_version = "RehabVoiceBridge/0.1"

    def log_message(self, format: str, *args: object) -> None:
        print(f"[{self.log_date_time_string()}] {format % args}")

    def send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        if self.path == "/health":
            self.send_json(HTTPStatus.OK, {"status": "ok"})
            return
        self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:
        if self.path != "/voice":
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= SAMPLE_RATE * 2 * 30:
                raise ServiceError("Recording must be 1 to 30 seconds of 16 kHz mono PCM.")
            if self.headers.get("Content-Type", "").split(";")[0] != "audio/pcm":
                raise ServiceError("Content-Type must be audio/pcm.")

            device_id = self.headers.get("X-Device-Id", "esp32-rehab-dev")
            pcm = self.rfile.read(length)
            transcript = transcribe(pcm, device_id)
            reply = generate_reply(transcript)
            audio = synthesize(reply, device_id)

            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "audio/basic;codec=pcm;rate=16000;channel=1")
            self.send_header("Content-Length", str(len(audio)))
            self.send_header("X-Transcript-Base64", base64.b64encode(transcript.encode()).decode())
            self.send_header("X-Reply-Base64", base64.b64encode(reply.encode()).decode())
            self.end_headers()
            self.wfile.write(audio)
            print(f"voice ok: transcript={transcript!r}; reply={reply!r}")
        except (ValueError, ServiceError) as error:
            print(f"voice error: {error}")
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})


def main() -> None:
    host = os.getenv("SERVER_HOST", "0.0.0.0")
    port = int(os.getenv("SERVER_PORT", "8000"))
    print(f"Local voice bridge listening on http://{host}:{port}")
    print("GET /health; POST /voice with 16 kHz 16-bit mono audio/pcm")
    ThreadingHTTPServer((host, port), VoiceHandler).serve_forever()


if __name__ == "__main__":
    main()
