# Cyber Ranges II

Cyber ranges II are realistic, segmented network environments designed to practice penetration testing, incident response, and adversary emulation in controlled settings. Each cyber range simulates corporate networks, industrial control systems, or specialized attack scenarios.

## Available Cyber Ranges

### 1. TechCorp Corporate Network (`easy_techcorp2`)
- **Difficulty**: Easy
- **Flags**: 4
- **Estimated Time**: 20-30 minutes
- **Focus**: Multi-tier corporate network with SQL injection, file upload RCE, SMTP enumeration, SSH pivoting, privilege escalation, and database exfiltration
- **Network Segments**: DMZ (192.168.10.0/24) and Backend (192.168.20.0/24)

### 2. Cobalt Group Ransomware Attack (`CobaltGroupRansomware`)
- **Difficulty**: Medium
- **Challenges**: 5 stages
- **Estimated Time**: 45-60 minutes
- **Focus**: Advanced adversary emulation simulating a Cobalt Group ransomware campaign with lateral movement across segmented networks
- **Network Segments**: Public Internet (172.20.0.0/24), DMZ (172.21.0.0/24), Office LAN (172.22.0.0/24), Server LAN (172.23.0.0/24)

---

## Setting Up and Starting a Cyber Range

### Step 1: Navigate to the Cyber Range Directory

```bash
# From the CAI repository root
cd src/cai/caibench/cyber_ranges/<cyber-range-name>

# For example:
cd src/cai/caibench/cyber_ranges/easy_techcorp2
# or
cd src/cai/caibench/cyber_ranges/CobaltGroupRansomware
```

### Step 2: Start the Environment

Start all containers using Docker Compose:

```bash
docker-compose up -d
```

**Wait for initialization**: Give containers 10-30 seconds to fully initialize before attacking.

### Step 3: Verify Containers are Running

```bash
docker-compose ps
```

You should see all containers in the "Up" state.

### Step 4: Check Logs (Optional)

```bash
# View logs for all services
docker-compose logs

# View logs for a specific service
docker-compose logs <service-name>
```

---

## Configuring CAI for Cyber Ranges

### Setting the Active Container

When working with cyber ranges, CAI agents operate from within a development container that serves as the "attacker position." You need to configure which container CAI should use.


1. **Development container network** (required for attack position)
   ```bash
   cd .devcontainer
   docker-compose up -d

   # The devcontainer_cainet network should exist
   docker network ls | grep cainet

   # If not present, some cyber ranges will create it automatically
   # or you may need to create it:
   docker network create --driver bridge --subnet 192.168.3.0/24 devcontainer_cainet
   ```

2. **Setting the attacker position**
   ```bash
   docker ps # Find ID for 

   #in .env add
   CAI_ACTIVE_CONTAINER="[CONTAINER ID]"
   ```

3. **CAI environment set up**
   ```bash
   # From the cai repository root
   source cai_env/bin/activate 

   #start cai as normal and prompt to agents
   ```

---
