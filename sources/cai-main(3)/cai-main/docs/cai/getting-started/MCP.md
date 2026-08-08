# MCP

CAI supports the Model Context Protocol (MCP) for integrating external tools and services with AI agents. Common patterns:

1. **STDIO (Standard Input/Output)** — For local processes, including the **Burp Suite MCP** stdio proxy from PortSwigger (extract `mcp-proxy-all.jar` from the MCP Server BApp; default Burp SSE URL is usually `http://127.0.0.1:9876`):

```bash
CAI>/mcp load stdio burp java -jar /path/to/mcp-proxy-all.jar --sse-url http://127.0.0.1:9876
```

2. **SSE (Server-Sent Events)** — Direct HTTP/SSE only works when the server sends a compliant `Content-Type: text/event-stream` response. Many tools (including Burp’s in-process SSE) are unreliable with CAI’s client; prefer **stdio** for Burp.

```bash
CAI>/mcp load http://127.0.0.1:8000/sse myserver
```

Other stdio servers:

```bash
CAI>/mcp load stdio myserver python mcp_server.py
```

Once connected, add the MCP tools to an agent (server name first, then agent id or index). The REPL prints a table of each tool and its status.

```bash
CAI>/mcp add burp redteam_agent
```

You can list all active MCP connections and their transport types:

```bash
CAI>/mcp list
```

Other useful subcommands: `/mcp status`, `/mcp associations`, `/mcp test <server>`, and `/mcp help` (same summary as `/help mcp` and `/h mcp`).

[https://github.com/user-attachments/assets/386a1fd3-3469-4f84-9396-2a5236febe1f](https://github.com/user-attachments/assets/386a1fd3-3469-4f84-9396-2a5236febe1f)

## Example: Controlling Chrome with CAI

1. Install node, following the instructions on the [official site](https://nodejs.org/en/download/current)
2. Install Chrome (Chromium is not compatible with this functionality)
3. Run the following commands:

```bash
CAI>/mcp load stdio devtools npx chrome-devtools-mcp@latest
CAI>/mcp add devtools redteam_agent
CAI>/agent redteam_agent
```

Once this is done, you will have full control of Chrome using the red team agent.