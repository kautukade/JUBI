# Packet capture on WSL2 and evidence artifacts

CAI agents capture traffic with `tcpdump`, `tshark`, or `dumpcap`. On **WSL2** (and some locked-down hosts), live capture often fails until the environment grants raw sockets.

## Fix capture permissions (WSL2 / Linux)

```bash
# One-time: allow dumpcap to capture without root
sudo setcap cap_net_raw+eip "$(command -v dumpcap)"

# Verify
getcap "$(command -v dumpcap)"
```

If `tcpdump` still fails, retry with `sudo tcpdump ...` when your policy allows it. CAI may prompt for sudo when tool output indicates missing privileges.

## Prefer CAI Docker (NET_RAW enabled)

CAI containers are started with `--cap-add=NET_RAW` for capture tools. Run network assessments inside the CAI container when host WSL lacks capabilities.

## Evidence types (what to ask the agent for)

| User asks for | Valid artifact | Invalid substitute |
|---------------|----------------|-------------------|
| PCAP | `.pcap` / `.pcapng` | `.txt` logs from curl/openssl in `packet_captures/` |
| Screenshot of traffic | Filtered PCAP or labeled `tshark` export | Text file in `screenshots/` |
| GUI / Wireshark window | Not available via shell agent | PNG rendered from text |

When capture fails, CAI prepends a **PACKET-CAPTURE FAILURE** notice to tool output. The agent should report the blocker and remediation—not fabricate captures.

## Filtered PCAPs (recommended)

```bash
tshark -r assessments/full.pcap -Y "http.request" -w assessments/filtered-pcaps/http_only.pcap
```

## CSV inventories (e.g. PAsset-XX)

Use the **Risk & Compliance** agent tool `verify_csv_inventory`:

- Pass the CSV path and your latest assessment text.
- Require `MISSING from response: none` before closing the task.

Example user prompt:

> List every PAsset-XX in `assets.csv`, assess each, run `verify_csv_inventory`, and do not finish until covered/total is complete.
