from __future__ import annotations

import base64
import unittest

from sarus.core.vision import VisionEngine


PNG_1X1 = base64.b64decode(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZkT0AAAAASUVORK5CYII='
)


class FakeModels:
    base = 'http://127.0.0.1:11434'

    def __init__(self):
        self.calls = []

    def list_models(self):
        return {
            'online': True,
            'models': ['qwen2.5vl:3b', 'qwen2.5:7b'],
            'items': [
                {'name': 'qwen2.5vl:3b', 'kind': 'vision'},
                {'name': 'qwen2.5:7b', 'kind': 'general'},
            ],
        }

    def choose(self, task_type='general'):
        return 'qwen2.5vl:3b' if task_type == 'vision' else 'qwen2.5:7b'

    def generate(self, prompt, **kwargs):
        self.calls.append((prompt, kwargs))
        return {'response': 'visible test result'}


class VisionTests(unittest.TestCase):
    def setUp(self):
        self.models = FakeModels()
        self.vision = VisionEngine(self.models)

    def test_status_is_local_only(self):
        status = self.vision.status()
        self.assertTrue(status['local_only'])
        self.assertEqual(status['selected_model'], 'qwen2.5vl:3b')
        self.assertEqual(len(status['vision_models']), 1)

    def test_valid_png_is_forwarded_to_local_ollama_images(self):
        encoded = base64.b64encode(PNG_1X1).decode('ascii')
        out = self.vision.analyze(encoded, 'What is visible?')
        self.assertEqual(out['provider'], 'ollama-local')
        self.assertEqual(out['model'], 'qwen2.5vl:3b')
        self.assertEqual(out['mime'], 'image/png')
        self.assertEqual(self.models.calls[0][1]['task_type'], 'vision')
        self.assertEqual(len(self.models.calls[0][1]['images']), 1)

    def test_rejects_invalid_base64_and_unknown_format(self):
        with self.assertRaises(ValueError):
            self.vision.analyze('not base64%%%')
        with self.assertRaises(ValueError):
            self.vision.analyze(base64.b64encode(b'hello world').decode('ascii'))

    def test_rejects_non_vision_model(self):
        encoded = base64.b64encode(PNG_1X1).decode('ascii')
        with self.assertRaises(ValueError):
            self.vision.analyze(encoded, model='qwen2.5:7b')


if __name__ == '__main__':
    unittest.main(verbosity=2)
