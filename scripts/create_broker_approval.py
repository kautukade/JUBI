from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import time


def canonical_hash(parameters: dict) -> str:
    raw = json.dumps(parameters, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
    return hashlib.sha256(raw).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description='Create a short-lived SARUS privileged broker approval proof.')
    ap.add_argument('--request-id', required=True)
    ap.add_argument('--action-id', required=True)
    ap.add_argument('--parameters-json', default='{}', help='Exact JSON parameters from the broker request')
    ap.add_argument('--ttl', type=int, default=120, help='Approval lifetime in seconds (1-300)')
    args = ap.parse_args()

    secret = os.environ.get('SARUS_BROKER_APPROVAL_SECRET', '')
    if len(secret) < 24:
        raise SystemExit('SARUS_BROKER_APPROVAL_SECRET must be configured with at least 24 characters.')

    try:
        parameters = json.loads(args.parameters_json)
    except json.JSONDecodeError as exc:
        raise SystemExit(f'Invalid --parameters-json: {exc}') from exc
    if not isinstance(parameters, dict):
        raise SystemExit('--parameters-json must decode to a JSON object.')

    ttl = max(1, min(args.ttl, 300))
    expires = int(time.time()) + ttl
    phash = canonical_hash(parameters)
    message = f'v1|{args.request_id}|{args.action_id}|{phash}|{expires}'.encode('utf-8')
    mac = hmac.new(secret.encode('utf-8'), message, hashlib.sha256).hexdigest()
    print(f'v1:{expires}:{mac}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
