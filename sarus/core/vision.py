from __future__ import annotations

import base64
import binascii
import hashlib
import time
from pathlib import Path


class VisionEngine:
    """Local image analysis through an installed Ollama vision model.

    Image bytes are validated and passed only to the local Ollama API. This
    component does not upload images to OpenRouter/NVIDIA/Hugging Face. Cloud
    vision can be added later only behind an explicit provider/privacy choice.
    """

    MAX_IMAGE_BYTES = 8 * 1024 * 1024

    def __init__(self, models, event_bus=None):
        self.models = models
        self.event_bus = event_bus

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is None:
            return
        try:
            self.event_bus.emit(kind, payload)
        except Exception:
            pass

    @staticmethod
    def _decode(value: str) -> tuple[bytes, str]:
        text = str(value or '').strip()
        mime = ''
        if text.startswith('data:'):
            try:
                head, text = text.split(',', 1)
            except ValueError as exc:
                raise ValueError('invalid image data URI') from exc
            if ';base64' not in head:
                raise ValueError('image data URI must be base64 encoded')
            mime = head[5:].split(';', 1)[0].strip().lower()
        try:
            raw = base64.b64decode(text, validate=True)
        except (binascii.Error, ValueError) as exc:
            raise ValueError('invalid base64 image') from exc
        if not raw:
            raise ValueError('image is empty')
        if len(raw) > VisionEngine.MAX_IMAGE_BYTES:
            raise ValueError('image exceeds 8 MiB limit')
        return raw, mime

    @staticmethod
    def _detect(raw: bytes, declared: str = '') -> str:
        if raw.startswith(b'\x89PNG\r\n\x1a\n'):
            actual = 'image/png'
        elif raw.startswith(b'\xff\xd8\xff'):
            actual = 'image/jpeg'
        elif len(raw) >= 12 and raw[:4] == b'RIFF' and raw[8:12] == b'WEBP':
            actual = 'image/webp'
        else:
            raise ValueError('only PNG, JPEG and WebP images are supported')
        if declared and declared not in {actual, 'image/jpg'}:
            raise ValueError('declared image type does not match image bytes')
        return actual

    def status(self) -> dict:
        status = self.models.list_models()
        vision = [x for x in status.get('items', []) if x.get('kind') == 'vision']
        return {
            'local_only': True,
            'ollama_online': bool(status.get('online')),
            'vision_models': vision,
            'selected_model': self.models.choose('vision') if status.get('online') else None,
            'max_image_bytes': self.MAX_IMAGE_BYTES,
            'formats': ['image/png', 'image/jpeg', 'image/webp'],
        }

    def analyze(self, image_base64: str, prompt: str = '', model: str | None = None, timeout: int = 300) -> dict:
        raw, declared = self._decode(image_base64)
        mime = self._detect(raw, declared)
        selected = str(model or '').strip() or self.models.choose('vision')
        if not selected:
            raise RuntimeError('no installed local Ollama vision model is available')
        installed = self.models.list_models()
        items = {x.get('name'): x for x in installed.get('items', []) if x.get('name')}
        if selected not in items:
            raise RuntimeError(f'the selected vision model {selected!r} is not installed')
        if items[selected].get('kind') != 'vision':
            raise ValueError('selected model is not classified as a vision model')
        question = str(prompt or '').strip() or 'Describe this image accurately and point out any important details.'
        started = time.perf_counter()
        encoded = base64.b64encode(raw).decode('ascii')
        self._emit(
            'VISION_ANALYSIS_STARTED',
            {'model': selected, 'bytes': len(raw), 'mime': mime, 'sha256': hashlib.sha256(raw).hexdigest()},
        )
        result = self.models.generate(
            question,
            task_type='vision',
            system=(
                'You are Jubi Vision. Analyze only what is visible in the supplied image. '
                'Do not invent hidden facts. When uncertain, say what is uncertain.'
            ),
            model=selected,
            timeout=timeout,
            images=[encoded],
        )
        elapsed = round((time.perf_counter() - started) * 1000.0, 2)
        out = dict(result or {})
        out['model'] = selected
        out['provider'] = 'ollama-local'
        out['mime'] = mime
        out['image_bytes'] = len(raw)
        out['latency_ms'] = elapsed
        self._emit('VISION_ANALYSIS_COMPLETED', {'model': selected, 'latency_ms': elapsed})
        return out
