# Claude Code Second Brain Skills

A collection of custom skills that turn Claude Code into a second brain for knowledge work. These skills demonstrate progressive disclosure of context - there's no magic, just structured knowledge that makes Claude hyper-capable for specific tasks.

## What This Is

Most people think of Claude Code as a tool for writing and debugging code. These skills extend it into a system for capturing and operationalizing knowledge:

- **External integrations** - Connect to MCP servers (Zapier, GitHub, etc.) without context bloat
- **Video creation** - Programmatic videos with Remotion and React
- **Presentations** - Professional slides and LinkedIn carousels with your brand styling
- **Documentation** - Runbooks, SOPs, and technical docs that people actually follow
- **Skill development** - Creating new skills to extend Claude's capabilities further
- **Brand consistency** - Define your voice and visual identity once, use everywhere

The system works through progressive disclosure: Claude only loads detailed instructions when needed, keeping context efficient while maintaining deep expertise for each domain.

---

## Brand & Voice Generator

Generate tone-of-voice and brand-system files that power the PPTX Generator and can guide customization of all the other skills. This skill walks you through creating your brand identity, defining your writing voice, and establishing your visual system.

**Core Philosophy**: Your brand and voice should be documented once and reused everywhere. The files this skill creates become the source of truth for all content generation.

<details>
<summary><big>📦 What It Creates</big></summary>

| File | Purpose | Used By |
|------|---------|---------|
| `brand.json` | Colors, fonts, assets | PPTX Generator |
| `config.json` | Output settings | PPTX Generator |
| `brand-system.md` | Design philosophy & guidelines | All skills |
| `tone-of-voice.md` | Writing voice & personality | PPTX content, SOPs |

</details>

<details>
<summary><big>🔄 Process Overview</big></summary>

1. **Gather Brand Basics** - Name, description, primary use case
2. **Define Colors** - 10 color values for the complete system
3. **Define Typography** - Heading, body, and code fonts
4. **Define Assets** - Logo and icon paths
5. **Discover Voice** - Personality, vocabulary, sentence patterns
6. **Create Design Philosophy** - Core principles and signature elements
7. **Generate Files** - Create all four files with gathered information

</details>

<details>
<summary><big>🎭 Voice Templates Included</big></summary>

The skill includes 5 example voice configurations to help you discover your own:

- **Technical Educator** - Enthusiastic expert who teaches by showing
- **Calm Authority** - Confident and measured, lets expertise speak through specifics
- **Builder's Perspective** - Developer-to-developer, unfiltered opinions backed by code
- **Approachable Expert** - Makes the complex accessible without dumbing it down
- **Contrarian Thinker** - Challenges conventional wisdom with evidence

</details>

<br/>

**Triggers**: "help me create a brand system", "generate my tone of voice", "set up my brand for presentations", "create brand files"

---

## Skills Overview

### MCP Client

Connect Claude Code to external MCP servers (Zapier, GitHub, Sequential Thinking, etc.) with progressive disclosure - tool schemas load on-demand instead of bloating your context window.

**Core Philosophy**: MCP servers expose thousands of tokens worth of tool definitions. This skill wraps them as a lightweight client, loading only what you need when you need it.

<details>
<summary><big>🔧 Setup: Create Your Config</big></summary>

**Step 1:** Copy the example config to create your own:

```bash
cp .claude/skills/mcp-client/references/example-mcp-config.json \
   .claude/skills/mcp-client/references/mcp-config.json
```

**Step 2:** Edit `mcp-config.json` with your API keys and servers.

The config format is identical to Claude Desktop's MCP config:

```json
{
  "mcpServers": {
    "zapier": {
      "url": "https://mcp.zapier.com/api/v1/connect",
      "api_key": "YOUR_API_KEY_HERE"
    },
    "sequential-thinking": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-sequential-thinking"]
    }
  }
}
```

**Transport types:**
- `url` + `api_key` → Remote server with Bearer auth (Zapier)
- `command` + `args` → Local stdio server (npx, python, docker)
- `url` ending in `/sse` → SSE transport
- `url` ending in `/mcp` → Streamable HTTP

</details>

<details>
<summary><big>📋 Available Commands (Agent Takes Care of This)</big></summary>

```bash
# List configured servers
python .claude/skills/mcp-client/scripts/mcp_client.py servers

# List tools from a server (with full schemas)
python .claude/skills/mcp-client/scripts/mcp_client.py tools zapier

# Call a tool
python .claude/skills/mcp-client/scripts/mcp_client.py call zapier <tool_name> '{"param": "value"}'
```

</details>

<details>
<summary><big>📝 Document Tool Gotchas in CLAUDE.md</big></summary>

**Important:** After setting up MCP servers, ask Claude to test each tool and document any quirks. This saves time on future calls.

Add a section to your project's `CLAUDE.md` (or create one) - example:

```markdown
## MCP Tool Notes

### Zapier
- `send_gmail_email`: The `to` field must be a single email, not an array
- `create_notion_page`: Requires `database_id`, not `page_id`
- Rate limit: 2 Zapier tasks per MCP call

### Sequential Thinking
- Always set `nextThoughtNeeded: true` until final thought
- `totalThoughts` is advisory, can be adjusted mid-process
```

**Why this matters:** MCP tools often have undocumented argument requirements or behaviors. Testing once and documenting saves context and prevents repeated errors.

**Workflow:**
1. Connect a new MCP server
2. Ask Claude: "List all tools from [server] and test each one with sample inputs"
3. Document any failures, required formats, or gotchas in CLAUDE.md
4. Claude will reference these notes on future calls

</details>

<br/>

**Example Triggers**: "connect to Zapier", "use MCP server", "list MCP tools", "call Zapier action", or any MCP server interaction

---

### PPTX Generator

Generate professional, on-brand presentation slides and LinkedIn carousels using python-pptx.

> **Credit**: This skill was originally created by Rasmus and is [maintained here](https://github.com/Wirasm/presentation-slides-skill). The version here has been adapted with brand-specific configurations.

<details>
<summary><big>🎛️ Three Operating Modes</big></summary>

1. **Slide Generation** - Create 16:9 presentations with brand styling
2. **Carousel Generation** - Create square 1:1 LinkedIn carousels (exports to PDF)
3. **Layout Management** - Create, edit, and improve cookbook layouts

</details>

<details>
<summary><big>✨ Key Features</big></summary>

- 16 slide layout templates in the cookbook (title, content, stats, two-column, multi-card, floating-cards, circular-hero, quote, chart, code, and more)
- 5 carousel-specific layouts (hook, single-point, numbered-point, quote, CTA)
- Brand system with colors, fonts, and assets
- Batch generation (max 5 slides at a time) for reliability
- Variety enforcement rules to prevent repetitive layouts

</details>

<details>
<summary><big>💡 Critical Concept</big></summary>

**Visual-first layout selection** - content-slide is the LAST RESORT, not the default.

The skill includes a decision tree to transform bullets into visual layouts:
- 3-5 equal items → multi-card-slide
- 2-4 big numbers → stats-slide
- Comparing two things → two-column-slide
- Central concept with surrounding items → circular-hero-slide
- Powerful quote → quote-slide

Only use content-slide when none of the visual layouts fit.

</details>

<br/>

**Triggers**: Requests for slides, presentations, carousels, PPTX, or layouts with a brand name

---

### SOP Creator

Create runbooks, playbooks, and technical documentation that people actually follow.

**Core Philosophy**: Nobody reads 50-page docs. Make it scannable, actionable, and impossible to misunderstand.

<details>
<summary><big>📂 Document Types</big></summary>

- **Tech/Engineering**: Runbook, Deployment Playbook, Troubleshooting Guide, How-To, ADR
- **Operations/Business**: Process SOP, Checklist, Decision Tree, Handoff Doc
- **Content/Creative**: Production Workflow, Review Process, Publishing Checklist
- **General**: Standard SOP, Quick Reference, Onboarding Guide

</details>

<details>
<summary><big>🏗️ Universal Structure</big></summary>

1. Definition of Done (checklist - most important, put near the top)
2. When to Use This
3. Prerequisites
4. The Process (numbered steps)
5. Verify Completion
6. When Things Go Wrong
7. Questions?

</details>

<details>
<summary><big>📏 Writing Rules</big></summary>

- Be specific (numbers, names, thresholds - not "as needed" or "regularly")
- Action-first steps (verbs, not descriptions)
- Warnings come first (before the dangerous step, not after)
- Clear decision points (if X, then Y - not "handle based on priority")

</details>

<br/>

**Triggers**: Requests to document a process, create a runbook, build operational docs, formalize technical procedures

---

### Skill Creator

Guide for creating effective skills that extend Claude's capabilities.

**Core Philosophy**: Skills are modular, self-contained packages that transform Claude from a general-purpose agent into a specialized agent. Only add context Claude doesn't already have.

<details>
<summary><big>🎁 What Skills Provide</big></summary>

1. Specialized workflows - Multi-step procedures for specific domains
2. Tool integrations - Instructions for working with specific file formats or APIs
3. Domain expertise - Company-specific knowledge, schemas, business logic
4. Bundled resources - Scripts, references, and assets for complex tasks

</details>

<details>
<summary><big>🧭 Core Principles</big></summary>

- **Concise is Key** - Only add context Claude doesn't already have
- **Set Appropriate Degrees of Freedom** - Match specificity to task fragility
- **Progressive Disclosure** - Metadata always in context, body when triggered, resources as needed

</details>

<details>
<summary><big>🗂️ Skill Anatomy</big></summary>

```
skill-name/
├── SKILL.md (required)
│   ├── YAML frontmatter (name, description)
│   └── Markdown instructions
└── Bundled Resources (optional)
    ├── scripts/     - Executable code
    ├── references/  - Documentation loaded as needed
    └── assets/      - Files used in output
```

</details>

<details>
<summary><big>🔄 Creation Process</big></summary>

1. Understand with concrete examples
2. Plan reusable contents
3. Initialize (run init_skill.py)
4. Edit and implement
5. Package (run package_skill.py)
6. Iterate based on real usage

</details>

**Triggers**: Requests to create or update skills that extend Claude's capabilities

---

### Remotion Video Creator

Create programmatic videos using React with [Remotion](https://remotion.dev). This skill gives Claude expert knowledge of the Remotion framework - animations, compositions, assets, captions, and more.

> **Credit**: This skill is from the official [remotion-dev/skills](https://github.com/remotion-dev/skills) repository by Jonny Burger and the Remotion team.

**Core Philosophy**: Videos are React components. Claude writes the code, you see it render in real-time, then export to MP4/WebM.

<details>
<summary><big>🚀 Setup: Create a Remotion Project</big></summary>

**Step 1:** Create a new Remotion project:

```bash
npx create-video@latest
```

When prompted, select:
- **Empty** template
- **Enable TailwindCSS** (recommended)
- **Install Skills** (this adds the Remotion skill automatically)

**Step 2:** Start the development server:

```bash
cd my-video
npm install
npm run dev
```

This opens a browser preview where you'll see your video render in real-time.

**Step 3:** Point Claude to your project:

```
"Edit my Remotion project at /path/to/my-video"
```

Claude will read the project structure and edit the React components in `src/`.

</details>

<details>
<summary><big>🎬 What You Can Create</big></summary>

The skill includes 28 modular rules covering:

- **Animations** - Interpolation, springs, easing curves
- **Compositions** - Multi-scene videos, dynamic duration
- **Assets** - Images, videos, audio, fonts, GIFs, Lottie
- **Text** - Typewriter effects, word highlights, measuring text
- **Captions** - TikTok-style captions, SRT import, transcription
- **Charts** - Animated bar charts, data visualization
- **3D** - Three.js and React Three Fiber integration
- **Transitions** - Scene transitions, sequencing patterns
- **Maps** - Animated Mapbox maps

</details>

<details>
<summary><big>💡 Tips for Prompting</big></summary>

**Good prompts** describe what can be drawn in a web interface:
- "Create a 10-second intro with my logo fading in, then text typing out"
- "Add a bar chart that animates the values from 0 to their final state"
- "Make the title bounce in with a spring animation"

**Don't** asking Claude to produce an entire video - one composition at a time is the way to go.

</details>

<br/>

**Triggers**: Mentions of "Remotion", paths containing `remotion-videos/`, files like `remotion.config.ts`, or requests for animated React video content

---

## Customization Guide

These skills work out of the box, but some contain brand-specific configurations you'll want to customize for your own use.

### What Needs Customization

| Skill | Customization Required | Effort |
|-------|------------------------|--------|
| **SOP Creator** | Works as-is | None |
| **Skill Creator** | Works as-is | None |
| **Remotion** | Create project externally | Low |
| **MCP Client** | Create config + add API keys | Low |
| **PPTX Generator** | Brand system setup required | Medium |

<details>
<summary><big>🟢 No Effort: SOP Creator & Skill Creator</big></summary>

These skills focus on universal principles and workflows that work for anyone. Use them immediately without any configuration.

</details>

<details>
<summary><big>🟡 Low Effort: Remotion</big></summary>

1. Run `npx create-video@latest` to create a Remotion project
2. Select Empty template + TailwindCSS + Install Skills
3. Start `npm run dev` and tell Claude the project path

The skill provides domain knowledge - the project itself lives outside this repo.

</details>

<details>
<summary><big>🟡 Low Effort: MCP Client</big></summary>

1. Copy `example-mcp-config.json` to `mcp-config.json`
2. Add your API keys (Zapier, GitHub tokens, etc.)
3. Test each server's tools and document gotchas in your `CLAUDE.md`

The config format matches Claude Desktop, so you can reuse existing configs.

</details>

<details>
<summary><big>🟡 Medium Effort: PPTX Generator</big></summary>

This skill requires a complete brand setup before generating slides. Use the **Brand & Voice Generator** skill to create these files interactively, or manually copy from the template folder.

**Required files** (in `brands/your-brand-name/`):

1. **`brand.json`** - Colors (10 values), fonts (3), and asset paths
2. **`config.json`** - Output directory, batch size, file naming
3. **`brand-system.md`** - Design philosophy, color rationale, typography rules, signature elements
4. **`tone-of-voice.md`** - Voice character, vocabulary patterns, do's and don'ts, example phrases

**Setup steps**:
1. Run the Brand & Voice Generator skill, OR
2. Copy `brands/template/` to `brands/your-brand-name/`
3. Replace all `REPLACE` placeholders in each file
4. Add your logo to `brands/your-brand-name/assets/`
5. Test by generating a simple presentation

See `brands/dynamous/` for a complete example of a configured brand.

</details>

---

## Quick Start

1. **For SOPs and documentation**: Use the SOP Creator directly - it works out of the box
2. **For new skills**: Use the Skill Creator to build your own extensions
3. **For MCP integrations**:
   - Copy `example-mcp-config.json` to `mcp-config.json` and add your API keys
   - Ask Claude to test the tools and document any gotchas in `CLAUDE.md`
4. **For videos**:
   - Run `npx create-video@latest` to create a Remotion project
   - Start the dev server with `npm run dev`
   - Tell Claude the project path and start prompting
5. **For presentations**:
   - First, run the Brand & Voice Generator to set up your brand
   - Or manually copy and configure the template folder in `brands/`
   - Then generate slides with the PPTX Generator skill

---

*These skills demonstrate that extending Claude Code is straightforward - it's just well-organized context that makes the agent an expert in your specific workflows.*
