# Case study: Network evidence and compliance inventories (field operator feedback)

This case study summarizes feedback from field use of CAI on **WSL2 / Linux** for SCM assessments by a partner certification body: PCAP capture, “screenshots” of traffic, and CSV privacy-asset reviews. It is written for operators and support — not as marketing material.

## Scenario

| Goal | What went wrong (v1.1.x) | Root cause |
|------|--------------------------|------------|
| PCAP per service/port | `.txt` files under `packet_captures/` | `CAP_NET_RAW` failure → model substituted openssl/curl logs |
| Screenshots of notable frames | `.txt` in `screenshots/`, later PNG from text | Shell agents have no Wireshark GUI; model improvised |
| Assess all PAsset-XX in CSV | Partial lists over multiple turns | LLM batching + long context; no deterministic checklist |

## What CAI 1.1.0 improves (artifact evidence update)

1. **Prompt + tool contract** — Only `.pcap`/`.pcapng` count as packet captures; screenshot wording reserved for real GUI capture or user-approved diagrams.
2. **Capture failure banner** — Tool output includes remediation (`setcap`, Docker `NET_RAW`) and forbids text substitutes.
3. **`verify_csv_inventory`** — Compliance agent can compare CSV IDs vs assessment text before closing.
4. **Bounded capture prompts** — Documentation stresses `timeout` and `-c` so `tcpdump` does not run indefinitely.

## Operator playbook (recommended prompts)

### Live PCAP (one host / port)

```text
Capture HTTPS to <TARGET_IP>: use timeout 15 tcpdump -i <IFACE> -c 200 -s 0 -w assessments/<name>.pcap "host <TARGET_IP> and port 443", then ls -lh and file that pcap. Do not leave tcpdump running indefinitely. If capture fails, report CAP_NET_RAW remediation — do not create .txt substitutes.
```

Generate traffic during the window: `curl -vk https://<TARGET_IP>/`

### Filtered PCAP instead of “screenshot”

```text
From assessments/<full>.pcap, write filtered PCAPs under assessments/filtered-pcaps/ for TLS Client Hello and HTTP GET only (tshark -r … -Y … -w …). Do not render text as PNG screenshots.
```

### Full CSV inventory (PAsset-XX)

```text
Assess every PAsset-XX in <file>.csv. Before finishing, run verify_csv_inventory with that file and your full assessment in response_text. Report covered/total; list any missing IDs and complete them.
```

## One-time host setup (WSL2 / Linux)

```bash
sudo setcap cap_net_raw+eip "$(command -v dumpcap)"
sudo setcap cap_net_raw+eip "$(command -v tcpdump)"
getcap "$(command -v tcpdump)"
```

Use CAI Docker with `NET_RAW` when host `setcap` is not allowed by policy.

## What CAI still cannot do

See [Platform limitations](../troubleshooting/platform_limitations.md). Summary:

- **Wireshark GUI screenshots** via shell agents.
- **Guaranteed all-in-one-pass** review of very large CSVs without chunking + `verify_csv_inventory`.
- **Grant CAP_NET_RAW** without operator or IT action on the host.

## Verification

- Regression tests: `tests/tools/test_capture_notice.py`, `test_evidence_inventory_check.py`
- Manual: [Operator feedback reproduction](../troubleshooting/operator_feedback_reproduction.md)

## References

- Session logs: `nopcap-onlytxt.zip`, `txt-to-png.zip` (May 2026, WSL2, alias1 model, Network / Compliance agents).
