# SARUS integration test report

- 10/10 repository adapters: PASS
- Capability registry covers all 17,356 original scanned files: PASS in assembled build workspace
- Cross-repository orchestration route includes all ten sources: PASS
- CAI non-readonly actions isolation gate: PASS
- High-risk approval gate: PASS
- Local Ollama role configuration: PASS
- Dashboard `/api/status`: PASS
- Dashboard `/api/capabilities`: PASS
- Event bus `/api/events`: PASS
- Plan pipeline `/api/plan`: PASS
- Extended regression suite: 17/17 PASS
- Bundled SARA project tests: 33/33 PASS

## Not certified in Linux build environment
Windows UI automation, camera/microphone, Windows Hello, GPU workloads, actual Ollama inference against the user's target machine, browser credentials, messaging/email accounts, cloud APIs, real deployments, kernel/driver experiments, and active cybersecurity lab operations. These require target Windows hardware/accounts and must not be represented as verified merely because their source repository is connected.
