from __future__ import annotations

import base64
import io
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.voice import VPSVoice


def wav_bytes():
    buf = io.BytesIO()
    with wave.open(buf, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(16000)
        out.writeframes(b"\x00\x00" * 1600)
    return buf.getvalue()


class FakeWhisper:
    def transcribe(self, path, **kwargs):
        self.path = path
        self.kwargs = kwargs
        return (
            iter([
                SimpleNamespace(text=" Hey Jubi"),
                SimpleNamespace(text=" open my task list"),
            ]),
            SimpleNamespace(language="en", language_probability=0.99),
        )


class VPSVoiceTests(unittest.TestCase):
    def test_audio_validation_and_wake_phrase(self):
        raw = wav_bytes()
        encoded = base64.b64encode(raw).decode("ascii")
        decoded, mime, suffix = VPSVoice._decode_audio(encoded, "audio/wav")
        self.assertEqual(decoded, raw)
        self.assertEqual(mime, "audio/wav")
        self.assertEqual(suffix, ".wav")
        self.assertTrue(VPSVoice._wake("Hey Jubi, open my task list"))
        self.assertTrue(VPSVoice._wake("हे जुबी काम दाखव"))
        self.assertFalse(VPSVoice._wake("ordinary sentence"))
        with self.assertRaises(ValueError):
            VPSVoice._decode_audio(base64.b64encode(b"not-wav").decode(), "audio/wav")

    def test_transcribe_uses_local_whisper_and_reports_wake(self):
        voice = VPSVoice(object())
        fake = FakeWhisper()
        raw = wav_bytes()
        encoded = "data:audio/wav;base64," + base64.b64encode(raw).decode("ascii")
        with patch.object(voice, "_whisper", return_value=fake):
            result = voice.transcribe(encoded)
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["wake_detected"])
        self.assertIn("open my task list", result["text"])
        self.assertEqual(result["language"], "en")
        self.assertFalse(Path(fake.path).exists(), "temporary audio must be deleted")

    def test_tts_is_local_and_returns_valid_wav(self):
        voice = VPSVoice(object())

        def run(argv, **kwargs):
            output = Path(argv[argv.index("-w") + 1])
            output.write_bytes(wav_bytes())
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with patch("sarus.core.voice.shutil.which", return_value="/usr/bin/espeak-ng"), \
             patch("sarus.core.voice.subprocess.run", side_effect=run):
            result = voice.synthesize("Namaste Jubi", "hi", 170)
        self.assertTrue(result["ok"])
        self.assertEqual(result["mode"], "local-espeak-ng")
        self.assertTrue(result["audio"].startswith("data:audio/wav;base64,"))
        self.assertGreater(result["frames"], 0)

    def test_status_never_claims_direct_microphone_or_cloud(self):
        voice = VPSVoice(object())
        with patch("sarus.core.voice.importlib.util.find_spec", return_value=None), \
             patch("sarus.core.voice.shutil.which", return_value=None):
            status = voice.status()
        self.assertFalse(status["ready"])
        self.assertFalse(status["always_listening"])
        self.assertFalse(status["microphone_capture"])
        self.assertFalse(status["cloud_speech"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
