"""Hermes SDK bridge to Jubi's single inference boundary.

Loaded only inside the dedicated Hermes worker. This compatibility shim is
scoped to that process; it never patches the desktop/server Python process.
"""
from __future__ import annotations

import json
import sys
import threading
import time
import uuid
import copy
import os
import subprocess

from sarus.core.provider_policy import InferenceTransport, LOCAL_ONLY, ProviderPolicyError


def install_transport(agent_class, base_url: str, receipts: list[dict], max_tokens=512,
                      *, process_commands=(), max_calls=12, cancel_check=lambda: False, context_tokens=4096,
                      request_timeout=120):
    import httpx
    from openai import OpenAI

    LOCAL_ONLY.authorize('ollama', base_url + '/api/tags')
    permission = threading.local()
    call_lock = threading.Lock()
    call_count = 0
    failed = False

    def deny_bypass(event, args):
        if event in {'socket.connect', 'socket.getaddrinfo'} and not getattr(permission, 'sending', False):
            raise ProviderPolicyError('Hermes network I/O must use the Jubi inference boundary')
        if event in {'subprocess.Popen', 'os.system', 'os.exec', 'os.posix_spawn'}:
            if event == 'subprocess.Popen':
                # Windows audits the serialized command line, POSIX the argv.
                # Compare exact serialization AND executable; never tokenize a
                # shell string or authorize a prefix.
                command = args[1]
                for allowed in process_commands:
                    if isinstance(command, str):
                        if (os.name == 'nt' and command == subprocess.list2cmdline(allowed)
                                and (args[0] is None or os.path.normcase(str(args[0])) == os.path.normcase(allowed[0]))):
                            return
                    elif tuple(command) == allowed:
                        return
            raise ProviderPolicyError('Process is outside the approved fixed workspace commands')

    sys.addaudithook(deny_bypass)

    class Wire(InferenceTransport):
        def _send(self, *args, **kwargs):
            permission.sending = True
            try:
                return super()._send(*args, **kwargs)
            finally:
                permission.sending = False

    class Bridge(httpx.BaseTransport):
        def handle_request(self, request):
            nonlocal call_count, failed
            with call_lock:
                if failed or cancel_check() or call_count >= max_calls:
                    raise ProviderPolicyError('Hermes inference cancelled, failed, or call budget exhausted')
                call_count += 1
            if str(request.url) != base_url + '/v1/chat/completions':
                raise ProviderPolicyError('Hermes tried to change the approved local endpoint')
            body = json.loads(request.read())
            stream = bool(body.pop('stream', False))
            body.pop('stream_options', None)
            body['stream'] = False
            body['max_tokens'] = min(int(body.get('max_tokens') or max_tokens), max_tokens)
            started = time.monotonic()
            receipt = {'model': body.get('model'), 'policy_revision': LOCAL_ONLY.revision,
                       'endpoint': base_url, 'status': 'FAILED',
                       'tools': [t.get('function', {}).get('name') for t in body.get('tools', [])]}
            try:
                # Native Ollama request allows us to bound KV context as well
                # as output. OpenAI-compatible APIs cannot configure num_ctx.
                messages = copy.deepcopy(body['messages'])
                for message in messages:
                    for call in message.get('tool_calls') or []:
                        arguments = call.get('function', {}).get('arguments')
                        if isinstance(arguments, str):
                            call['function']['arguments'] = json.loads(arguments)
                native = {'model': body['model'], 'messages': messages, 'stream': False,
                          'options': {'num_ctx': context_tokens, 'num_predict': body['max_tokens'], 'num_gpu': 0},
                          'keep_alive': 30}
                native['options']['temperature'] = 0
                if body.get('tools'):
                    native['tools'] = body['tools']
                raw = Wire(base_url).json('/api/chat', native, timeout=request_timeout)
                if raw.get('prompt_eval_count', 0) >= context_tokens - 16:
                    raise ProviderPolicyError('Request filled the local context limit; refusing potentially truncated tool instructions')
                message = dict(raw.get('message') or {})
                for call in message.get('tool_calls') or []:
                    call.setdefault('id', 'call_' + uuid.uuid4().hex)
                    call.setdefault('type', 'function')
                    args = call.get('function', {}).get('arguments')
                    if isinstance(args, dict):
                        call['function']['arguments'] = json.dumps(args)
                data = {'id': 'chatcmpl-' + uuid.uuid4().hex, 'object': 'chat.completion',
                        'created': int(time.time()), 'model': body['model'],
                        'choices': [{'index': 0, 'message': message,
                                     'finish_reason': 'tool_calls' if message.get('tool_calls') else 'stop'}],
                        'usage': {'prompt_tokens': raw.get('prompt_eval_count', 0),
                                  'completion_tokens': raw.get('eval_count', 0),
                                  'total_tokens': raw.get('prompt_eval_count', 0) + raw.get('eval_count', 0)}}
                receipt.update(status='SUCCEEDED', usage=data.get('usage'),
                               native_tool_calls=len(message.get('tool_calls') or []))
            except Exception as exc:
                # Hermes may retry an API exception above the SDK. The task's
                # zero-retry policy is enforced here before any further I/O.
                with call_lock:
                    failed = True
                receipt['error'] = str(exc)[:500]
                raise
            finally:
                receipt['latency_ms'] = round((time.monotonic() - started) * 1000, 2)
                receipts.append(receipt)
            if not stream:
                return httpx.Response(200, json=data)
            # Preserve the SDK's stream contract while buffering at Jubi's
            # guarded transport. This pilot does not claim token streaming.
            choices = []
            for choice in data.get('choices', []):
                delta = dict(choice.get('message') or {})
                for index, call in enumerate(delta.get('tool_calls') or []):
                    call['index'] = index
                choices.append({'index': choice.get('index', 0), 'delta': delta,
                                'finish_reason': choice.get('finish_reason')})
            chunk = {**data, 'object': 'chat.completion.chunk', 'choices': choices}
            return httpx.Response(200, headers={'Content-Type': 'text/event-stream'},
                                  content=('data: ' + json.dumps(chunk) + '\n\ndata: [DONE]\n\n').encode())

    def create_client(agent, client_kwargs, *, reason, shared):
        if str(client_kwargs.get('base_url', '')).rstrip('/') != base_url + '/v1':
            raise ProviderPolicyError('Hermes provider override is outside the approved local endpoint')
        LOCAL_ONLY.check_model_name(agent.model)
        return OpenAI(base_url=base_url + '/v1', api_key='local-no-key', max_retries=0,
                      http_client=httpx.Client(transport=Bridge(), trust_env=False))

    if agent_class is not None:
        agent_class._create_openai_client = create_client
    return create_client
