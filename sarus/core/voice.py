"""Local/offline voice primitives for the Linux VPS profile.

The VPS cannot directly hear a laptop microphone. Instead, an authenticated
Jubi client uploads a bounded audio clip to the loopback API (normally through
the SSH tunnel). STT runs with a locally provisioned faster-whisper model and
TTS runs with local espeak-ng. No cloud speech provider is used here.
"""
from __future__ import annotations

import base64
import binascii
import importlib.util
import os
import re
import shutil
import subprocess
import tempfile
import threading
import wave
from pathlib import Path


class VPSVoice:
    MAX_AUDIO_BYTES = 20 * 1024 * 1024
    MAX_TTS_CHARS = 4000
    WAKE_PHRASES = ("hey jubi", "hi jubi", "हे जुबी", "हाय जुबी")

    _MIME_EXT = {
        "audio/wav": ".wav",
        "audio/x-wav": ".wav",
        "audio/wave": ".wav",
        "audio/mpeg": ".mp3",
        "audio/mp3": ".mp3",
        "audio/ogg": ".ogg",
        "audio/webm": ".webm",
        "audio/mp4": ".m4a",
        "audio/x-m4a": ".m4a",
    }

    def __init__(self, app):
        self.app = app
        self._model = None
        self._model_path = None
        self._lock = threading.Lock()

    @staticmethod
    def _model_dir() -> Path | None:
        raw = os.environ.get("JUBI_WHISPER_MODEL", "").strip()
        if not raw:
            return None
        path = Path(raw).expanduser().resolve()
        return path if path.is_dir() else None

    def status(self) -> dict:
        model_dir = self._model_dir()
        faster_whisper = importlib.util.find_spec("faster_whisper") is not None
        espeak = shutil.which("espeak-ng")
        ffmpeg = shutil.which("ffmpeg")
        return {
            "ready": bool(faster_whisper and model_dir and espeak and ffmpeg),
            "stt_ready": bool(faster_whisper and model_dir and ffmpeg),
            "tts_ready": bool(espeak),
            "faster_whisper": faster_whisper,
            "whisper_model": str(model_dir) if model_dir else None,
            "espeak_ng": espeak,
            "ffmpeg": ffmpeg,
            "mode": "offline-clip-stt-local-tts",
            "wake_phrase_detection": "post-transcription",
            "always_listening": False,
            "microphone_capture": False,
            "cloud_speech": False,
            "max_audio_bytes": self.MAX_AUDIO_BYTES,
            "max_tts_chars": self.MAX_TTS_CHARS,
        }

    @classmethod
    def _decode_audio(cls, value: str, mime_hint: str = "") -> tuple[bytes, str, str]:
        text = str(value or "").strip()
        mime = str(mime_hint or "").strip().lower()
        if text.startswith("data:"):
            try:
                head, text = text.split(",", 1)
            except ValueError as exc:
                raise ValueError("invalid audio data URI") from exc
            if ";base64" not in head:
                raise ValueError("audio data URI must be base64 encoded")
            declared = head[5:].split(";", 1)[0].strip().lower()
            if mime and declared != mime:
                raise ValueError("audio MIME hint does not match data URI")
            mime = declared
        if mime not in cls._MIME_EXT:
            raise ValueError("unsupported audio type; use WAV, MP3, OGG, WebM or M4A")
        try:
            raw = base64.b64decode(text, validate=True)
        except (binascii.Error, ValueError) as exc:
            raise ValueError("invalid base64 audio") from exc
        if not raw:
            raise ValueError("audio is empty")
        if len(raw) > cls.MAX_AUDIO_BYTES:
            raise ValueError("audio exceeds 20 MiB limit")
        if mime in {"audio/wav", "audio/x-wav", "audio/wave"} and not raw.startswith(b"RIFF"):
            raise ValueError("WAV audio does not contain a RIFF header")
        return raw, mime, cls._MIME_EXT[mime]

    def _whisper(self):
        model_dir = self._model_dir()
        if not model_dir:
            raise RuntimeError(
                "Local Whisper model is not provisioned. Run deploy/vps/provision-voice.sh --small."
            )
        try:
            from faster_whisper import WhisperModel
        except ImportError as exc:
            raise RuntimeError("faster-whisper is not installed; reinstall Jubi with --with-voice") from exc

        with self._lock:
            if self._model is not None and self._model_path == model_dir:
                return self._model
            device = os.environ.get("JUBI_WHISPER_DEVICE", "cpu").strip() or "cpu"
            compute_type = os.environ.get(
                "JUBI_WHISPER_COMPUTE_TYPE",
                "int8" if device == "cpu" else "float16",
            ).strip()
            self._model = WhisperModel(
                str(model_dir),
                device=device,
                compute_type=compute_type,
                local_files_only=True,
            )
            self._model_path = model_dir
            return self._model

    @classmethod
    def _wake(cls, text: str) -> bool:
        normalized = re.sub(r"[^\w\s\u0900-\u097f]", " ", str(text or "").lower())
        normalized = re.sub(r"\s+", " ", normalized).strip()
        return any(phrase in normalized for phrase in cls.WAKE_PHRASES)

    def transcribe(self, audio_base64: str, mime: str = "", language: str = "") -> dict:
        raw, mime, suffix = self._decode_audio(audio_base64, mime)
        model = self._whisper()
        tmp_path = None
        try:
            with tempfile.NamedTemporaryFile(prefix="jubi-voice-", suffix=suffix, delete=False) as tmp:
                tmp.write(raw)
                tmp_path = Path(tmp.name)
            segments, info = model.transcribe(
                str(tmp_path),
                language=str(language or "").strip() or None,
                beam_size=1,
                best_of=1,
                vad_filter=True,
                condition_on_previous_text=False,
            )
            pieces = []
            segment_count = 0
            for segment in segments:
                segment_count += 1
                pieces.append(str(segment.text or "").strip())
                if sum(len(x) for x in pieces) > 30_000:
                    break
            text = re.sub(r"\s+", " ", " ".join(x for x in pieces if x)).strip()
            if not text:
                raise RuntimeError("speech recognizer returned no text")
            return {
                "ok": True,
                "text": text,
                "language": getattr(info, "language", None),
                "language_probability": getattr(info, "language_probability", None),
                "segments": segment_count,
                "wake_detected": self._wake(text),
                "mode": "offline-faster-whisper",
                "audio_bytes": len(raw),
                "mime": mime,
            }
        finally:
            if tmp_path is not None:
                try:
                    tmp_path.unlink(missing_ok=True)
                except OSError:
                    pass

    @staticmethod
    def _voice_code(language: str) -> str:
        raw = str(language or "en").strip().lower().replace("_", "-")
        base = raw.split("-", 1)[0]
        aliases = {
            "english": "en", "hindi": "hi", "marathi": "mr",
            "en": "en", "hi": "hi", "mr": "mr",
        }
        code = aliases.get(raw, aliases.get(base, base))
        if not re.fullmatch(r"[a-z]{2,3}", code):
            raise ValueError("invalid TTS language")
        return code

    def synthesize(self, text: str, language: str = "en", speed: int = 165) -> dict:
        message = str(text or "").strip()
        if not message:
            raise ValueError("text is required")
        if len(message) > self.MAX_TTS_CHARS:
            raise ValueError("TTS text exceeds 4000 characters")
        espeak = shutil.which("espeak-ng")
        if not espeak:
            raise RuntimeError("espeak-ng is not installed; reinstall Jubi with --with-voice")
        voice = self._voice_code(language)
        rate = max(80, min(int(speed), 300))
        tmp_path = None
        try:
            fd, raw_path = tempfile.mkstemp(prefix="jubi-tts-", suffix=".wav")
            os.close(fd)
            tmp_path = Path(raw_path)
            cp = subprocess.run(
                [espeak, "-v", voice, "-s", str(rate), "-w", str(tmp_path), "--", message],
                capture_output=True,
                text=True,
                timeout=30,
                shell=False,
            )
            if cp.returncode != 0:
                raise RuntimeError("espeak-ng failed: " + (cp.stderr or cp.stdout)[-1000:])
            raw = tmp_path.read_bytes()
            if not raw.startswith(b"RIFF"):
                raise RuntimeError("TTS output is not a WAV file")
            # Parse enough of the WAV header to prove it is structurally readable.
            with wave.open(str(tmp_path), "rb") as wav:
                metadata = {
                    "channels": wav.getnchannels(),
                    "sample_rate": wav.getframerate(),
                    "frames": wav.getnframes(),
                }
            return {
                "ok": True,
                "audio": "data:audio/wav;base64," + base64.b64encode(raw).decode("ascii"),
                "mime": "audio/wav",
                "bytes": len(raw),
                "language": voice,
                "speed": rate,
                "mode": "local-espeak-ng",
                **metadata,
            }
        finally:
            if tmp_path is not None:
                try:
                    tmp_path.unlink(missing_ok=True)
                except OSError:
                    pass
