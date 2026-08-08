# CAI Queue File Feature

## Overview
CAI now supports automatically loading and executing a queue of prompts from a text file on startup. This is useful for batch processing, automated workflows, or pre-loading common tasks. The prompts will be executed automatically one by one without requiring user interaction.

## Usage

### 1. Set the Environment Variable
```bash
export CAI_QUEUE_FILE="/path/to/your/prompts.txt"
```

### 2. Create a Queue File
Create a text file with one prompt per line:

```text
# Comments start with # and are ignored
/help
What are your cybersecurity capabilities?
/agent list
Scan the network 192.168.1.0/24
Check for vulnerabilities in https://example.com
$ nmap -sV localhost
/history
Generate a security report
```

### 3. Start CAI
The prompts will be automatically loaded and executed when you start CAI:

```bash
# Regular mode - prompts run automatically
CAI_QUEUE_FILE=~/my_prompts.txt cai

# TUI mode - prompts run automatically
CAI_QUEUE_FILE=~/my_prompts.txt cai --tui
```

CAI will:
1. Load all prompts from the file
2. Display a message showing how many prompts were loaded
3. Automatically start processing each prompt in order
4. Return to normal interactive mode when the queue is empty

### 4. Manual Loading
You can also load a queue file manually using the queue command:

```bash
CAI> /queue load ~/another_prompts.txt
```

## Features

- **Auto-loading**: Queue file is loaded automatically on startup if `CAI_QUEUE_FILE` is set
- **Auto-execution**: Prompts are executed automatically in sequence without user interaction
- **Comments**: Lines starting with `#` are ignored
- **Empty lines**: Blank lines are skipped
- **Any prompt type**: Supports commands (`/help`), shell commands (`$ ls`), bare `?` (CLI headless input shortcuts), and regular prompts
- **Works in both modes**: Compatible with regular CLI and TUI modes
- **Seamless transition**: Returns to interactive mode after queue is processed

## Example Queue File

```text
# Security Assessment Workflow
# First, check the environment
/agent list
/model

# Network reconnaissance
Scan the network 192.168.1.0/24 for open ports
$ nmap -sV -p- 192.168.1.1

# Web application testing
Check https://example.com for common vulnerabilities
Test for SQL injection on the login form
Analyze the SSL/TLS configuration

# Reporting
Generate a comprehensive security report
/history
```

## Tips

- Use queue files to standardize workflows across team members
- Create different queue files for different types of assessments
- Combine with parallel mode for concurrent execution
- Queue items are processed in order, one at a time
- Add `/exit` at the end to quit CAI after processing
- You can interrupt processing at any time with Ctrl+C
- The queue continues from where it left off if you resume