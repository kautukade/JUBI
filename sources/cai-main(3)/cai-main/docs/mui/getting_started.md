# Getting Started with CAI Mobile UI

> **⚡ CAI-Pro Exclusive**  
> **[Join the TestFlight Beta](https://testflight.apple.com/join/nXZZD4Z5)** to get started with CAI Mobile UI.

This guide will walk you through installing, configuring, and using the CAI Mobile UI for the first time.

## Prerequisites

Before you begin, ensure you have:

1. **CAI-Pro License**: Active subscription from [Alias Robotics](https://aliasrobotics.com)
2. **iOS Device**: iPhone or iPad running iOS 15.0+
3. **CAI API Server**: Running CAI API server (v0.7.0+) on your network
4. **API Key**: Valid `ALIAS_API_KEY` for authentication

## Installation

### Step 1: Join TestFlight Beta

1. On your iOS device, visit: [https://testflight.apple.com/join/nXZZD4Z5](https://testflight.apple.com/join/nXZZD4Z5)
2. If prompted, install TestFlight from the App Store
3. Accept the beta testing invitation
4. Tap "Install" to download CAI Mobile UI

### Step 2: Launch the App

1. Find the CAI app icon on your home screen
2. Tap to launch
3. Grant necessary permissions when prompted:
   - **Local Network**: Required for discovering CAI servers
   - **Notifications**: Optional, for background task alerts

![CAI Mobile UI Login Screen](../media/MUI/cai_app_ios_login.png)

## Initial Setup

### Option A: Automatic Server Discovery

Perfect for local network setups:

1. **Start your CAI API server** on your computer:
   ```bash
   cai --api
   ```
   Note the server address (e.g., `http://192.168.1.100:8000`)

2. **On your iOS device**:
   - Ensure Wi-Fi is enabled and connected to the same network
   - Tap "Scan Network" on the login screen
   - Wait for the discovery process (usually 2-3 seconds)
   - Select your server from the list

3. **Enter your API key**:
   - Paste or type your `ALIAS_API_KEY`
   - Toggle "Remember Me" to save credentials
   - Tap "Connect"

### Option B: Manual Server Configuration

For remote servers or specific configurations:

1. **Server URL**:
   - Enter the complete URL (e.g., `https://cai.company.com:8443`)
   - Include the protocol (`http://` or `https://`)
   - Include the port if not standard

2. **API Key**:
   - Enter your `ALIAS_API_KEY`
   - Toggle "Remember Me" for convenience

3. **Advanced Options** (tap gear icon):
   - **Timeout**: Adjust connection timeout (default: 30s)
   - **SSL Verification**: Toggle for self-signed certificates
   - **Proxy**: Configure if needed

4. Tap "Connect"

## First Session

### 1. Welcome Screen

After successful connection, you'll see:
- Agent selector at the top
- Model selector below
- Empty chat interface
- Navigation tabs at bottom

### 2. Select an Agent

For your first session, we recommend:

1. Tap the agent selector
2. Choose `selection_agent` - it helps recommend the right agent for your task
3. Or select a specific agent like:
   - `red_teamer_agent` - For offensive security testing
   - `blue_teamer_agent` - For defensive analysis
   - `bug_hunter_agent` - For vulnerability discovery

### 3. Choose a Model

1. Tap the model dropdown
2. Recommended models:
   - `gpt-4o` - Best overall performance
   - `claude-3.5-sonnet` - Excellent for code analysis
   - `alias1` - Optimized for security tasks
   - `cohere/command-r-plus-08-2024` - Great performance and value

### 4. Start Your First Conversation

![CAI Mobile UI Chat Interface](../media/MUI/cai_app_ios_chat.png)

Try these starter prompts:

**For Security Testing:**
```
Analyze the security of example.com
```

**For Learning:**
```
Explain how SQL injection works and how to prevent it
```

**For Agent Recommendation:**
```
I need to perform a penetration test on a web application. Which agent should I use?
```

### 5. Understanding Responses

As the agent responds, you'll see:

- **Streaming Text**: Responses appear in real-time
- **Formatted Output**: Code blocks, lists, and emphasis
- **Thinking Indicators**: When agents are processing
- **Tool Usage**: When agents use external tools

![CAI Mobile UI Reasoning Display](../media/MUI/cai_app_ios_reasoning.png)

## Essential Features

### Message Interactions

- **Copy Text**: Long press any message → Copy
- **Share Output**: Long press → Share → Choose app
- **Save Code**: Tap code blocks → Copy button
- **Retry Message**: Swipe left on your message → Retry

### Navigation

- **Switch Conversations**: Swipe left/right or use tab bar
- **New Conversation**: Tap + button
- **View History**: Tap clock icon
- **Return Home**: Tap CAI logo

### Quick Actions

- **Cancel Generation**: Pull down while response is streaming
- **Clear Chat**: Shake device → Clear option
- **Change Agent Mid-Chat**: Tap agent name → Select new
- **Export Session**: Menu → Export → Choose format

## Keyboard Shortcuts (iPad with External Keyboard)

| Shortcut | Action |
|----------|--------|
| `⌘ + N` | New conversation |
| `⌘ + W` | Close current chat |
| `⌘ + ←/→` | Switch conversations |
| `⌘ + K` | Quick agent switch |
| `⌘ + /` | Focus message input |
| `⌘ + ↑` | Previous message |

## Best Practices

### 1. Network Connection
- Use Wi-Fi when possible for better performance
- Enable "Low Data Mode" in settings for cellular
- Download conversations for offline viewing

### 2. Security
- Enable Face ID/Touch ID in settings
- Don't share screenshots with API keys visible
- Use secure connections (HTTPS) when possible

### 3. Performance
- Close unused conversations to free memory
- Enable "Reduce Motion" for older devices
- Clear cache periodically in settings

## Troubleshooting

### Common Issues

**Can't connect to server:**
- Verify server is running: `cai --api`
- Check firewall allows port 8000
- Ensure devices are on same network
- Try manual IP instead of discovery

**Authentication failed:**
- Regenerate API key: `cai --keys`
- Check key hasn't expired
- Verify key matches server configuration

**App crashes or freezes:**
- Force quit and restart app
- Check for app updates in TestFlight
- Clear app cache in settings
- Report issue with crash logs

### Getting Help

1. **In-App Help**: Tap menu → Help
2. **Documentation**: [https://docs.aliasrobotics.com](https://docs.aliasrobotics.com)
3. **Discord Community**: [Join Discord](https://discord.gg/aliasrobotics)
4. **Report Issues**: [GitHub Issues](https://github.com/aliasrobotics/cai/issues)

## Next Steps

Now that you're connected and running:

1. 📱 [Explore the User Interface](user_interface.md)
2. 👆 [Master Gestures & Shortcuts](gestures_shortcuts.md)
3. 💬 [Learn Advanced Chat Features](chat_features.md)
4. 🛠️ [Configure Network & MCP Tools](network_mcp.md)

---

*Welcome to CAI Mobile UI - Security testing in your pocket!*