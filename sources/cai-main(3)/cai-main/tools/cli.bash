# Script to set up various CLI assistants

# Using nvm (recommended)
curl -fsSL https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh | bash
# Restart your terminal or source nvm
source ~/.nvm/nvm.sh
# Install latest LTS version of Node.js
nvm install --lts

## Claude Code CLI
# Install claude code
npm install -g @anthropic-ai/claude-code

## Codex CLI
npm install -g @openai/codex
# then, 
#   codex login
#   codex -s workspace-write -a on-request -m gpt-5 -c model_reasoning_effort="high" --search