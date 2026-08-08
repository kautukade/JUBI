# MCP SSE Example

This example uses a local SSE server in [server.py](server.py).

Run the example via:

```
uv run python examples/mcp/sse_example/main.py
```

## Details

The example uses `MCPServerSse` from `agents.mcp` (see `main.py`; same MCP server types as in CAI’s `cai.sdk.agents.mcp`). The SSE URL must match your server (often `http://localhost:8000/sse`).
