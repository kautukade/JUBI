# Reproducing field operator feedback scenarios

This guide reproduces the PCAP / screenshot / inventory issues from May 2026 session logs shared by a partner certification body, and how to verify fixes in CAI **v1.1.0+** (artifact evidence changes on branch `cai-v1.1.0`).

## Attached logs (reference)

| Zip | Session | Main symptom |
|-----|---------|--------------|
| `nopcap-onlytxt.zip` | Compliance / SCM-1 | `CAP_NET_RAW` failure → `.txt` in `packet_captures/` |
| `txt-to-png.zip` | Network analyzer | `.txt` “screenshots” → ImageMagick PNGs |

## 1) PCAP permission failure notice

**Simulate (no real capture needed):**

```bash
cd cai
python3 -c "
from cai.tools.evidence.capture_notice import apply_packet_capture_notice
cmd = 'tcpdump -i any -w /tmp/t.pcap -c 1'
out = '''tcpdump: any: You don't have permission to perform this capture
(Attempt to create packet socket failed - CAP_NET_RAW may be required)'''
print(apply_packet_capture_notice(cmd, out))
"
```

**Expected:** output starts with `[CAI PACKET-CAPTURE FAILURE]`.

**Live on WSL without setcap:**

```bash
tcpdump -i any -c 1 -w /tmp/test.pcap 2>&1 | head -5
```

Run the same command via CAI `generic_linux_command`; the model should see the notice and must not write openssl/curl output into `packet_captures/`.

**Fix environment then retest:**

```bash
sudo setcap cap_net_raw+eip "$(command -v dumpcap)"
```

## 2) False screenshots (txt → png)

**Reproduce user prompt (network or CTF agent):**

> Initiate communications on open ports on 192.0.2.104, create pcaps per port under assessments/, and take screenshots of notable parts in the pcaps.

**Before fix:** agent writes `assessments/screenshots/*.txt` (tshark text).

**After user correction:** agent should prefer `assessments/filtered-pcaps/*.pcap` and must not claim ImageMagick PNGs are Wireshark GUI captures.

**Verify in workspace:**

```bash
find assessments -name '*.txt' -path '*/screenshots/*'
file assessments/real-screenshots/*.png 2>/dev/null | head -3
```

PNG files that are "PNG image data" but only contain rendered text are **diagrams**, not GUI screenshots—expected limitation.

## 3) CSV inventory completeness

```bash
cd cai
.venv/bin/python3 -m pytest \
  tests/tools/test_capture_notice.py \
  tests/tools/test_evidence_inventory_check.py \
  tests/tools/test_tool_generic_linux_command.py::test_packet_capture_failure_notice_detects_tcpdump_error \
  tests/tools/test_tool_generic_linux_command.py::test_packet_capture_failure_notice_skips_tshark_read_only \
  tests/tools/test_tool_generic_linux_command.py::test_generic_linux_command_prepends_capture_notice \
  -q --timeout=60
```

**Expected:** `8 passed` in under a few seconds. If pytest hangs after `....`, an old build was waiting for an interactive sudo password—upgrade to the branch that skips sudo retry when the packet-capture notice is already present, then Ctrl+C and re-run.

**Interactive test:**

1. Create `workspace/test_assets.csv` with `PAsset-01` … `PAsset-10`.
2. Ask Compliance agent to assess all; paste partial reply into `verify_csv_inventory` via tool call.
3. Confirm `MISSING` lists gaps.

## 4) Replay JSONL logs (read-only)

```bash
unzip -p ~/Downloads/nopcap-onlytxt.zip '*.jsonl' | \
  python3 -c "
import sys, json, re
for i, line in enumerate(sys.stdin, 1):
    if 'CAP_NET_RAW' in line or 'packet_captures' in line and '.txt' in line:
        print(i, line[:200])
" | head -20
```

This confirms permission errors and txt substitutes in the original session.

## What remains impossible (tell the operator)

See `docs/cai/troubleshooting/platform_limitations.md` for customer-facing explanations.
