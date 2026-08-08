# Platform limitations (customer-facing)

Items CAI **mitigates** (prompts, tool notices, `verify_csv_inventory`) but **cannot fully eliminate**.

## Desktop / Wireshark screenshots

**What users expect:** PNG of the Wireshark GUI (packet list, decode panes).

**What shell agents have:** `generic_linux_command`, optional `execute_code`—no display server, no `computer_screenshot` tool on CTF/network/compliance agents.

**What CAI can do:** Filtered PCAPs, `tshark` field exports, markdown summaries, optional text-rendered diagrams (clearly labeled).

**What to tell the operator:** Ask for *filtered PCAPs* or *tshark export of frames X–Y*, not GUI screenshots, unless you run a separate desktop automation stack.

## 100% LLM rule compliance

Prompts and tool banners reduce wrong substitutions; models may still occasionally ignore them under long contexts or repeated interruptions.

**Mitigation:** Short, explicit tasks; verify artifacts on disk (`file *.pcap`, `verify_csv_inventory`).

**Not a bug:** Residual hallucination risk is inherent to LLM agents.

## CAP_NET_RAW on WSL2

**Cause:** Linux capability not granted to `dumpcap`/`tcpdump` in the WSL VM.

**What CAI does:** Detect failure, suggest `setcap`, sudo, or Docker; trigger sudo prompt when TTY allows.

**What CAI cannot do:** Grant kernel capabilities without the user (or installer) configuring the host.

**Action for the operator:** `sudo setcap cap_net_raw+eip $(which dumpcap)` or use CAI Docker with `NET_RAW`.

## Very large CSV inventories

**Cause:** Context limits; model may stop after partial batches even with good prompts.

**What CAI added:** `verify_csv_inventory` tool on Compliance agent—deterministic missing-ID list.

**What still helps:** Split CSV by chapter; run verify after each batch; merge results.

**Not solved by prompts alone** for multi-thousand-row sheets without chunking.

## Agent interruption (`SYSTEM CONTEXT NOTE`)

When the user switches agents or tasks, CAI injects a note to prioritize the new request. Earlier work may stop mid-flight.

**Not the PCAP bug**—by design. Use “resume previous task” if continuation is intended.
