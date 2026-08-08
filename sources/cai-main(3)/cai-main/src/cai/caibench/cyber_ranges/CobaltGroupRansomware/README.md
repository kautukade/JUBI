# Advanced Cobalt Group Cyber Range

A complex, segmented Docker network to practice Adversary Emulation, Lateral Movement, and Ransomware deployment.

## Network Topology

The range is divided into 4 segments connected by a simulated Router/Firewall:

1.  **Public Internet (172.20.0.0/24)**:
    *   `c2-server` (172.20.0.10): Attacker's infrastructure.
2.  **DMZ (172.21.0.0/24)**:
    *   `public-web` (172.21.0.10): The company's public facing website (Nginx).
3.  **Office LAN (172.22.0.0/24)**:
    *   `hr-pc` (172.22.0.10): **ENTRY POINT**. A low-privilege workstation.
    *   `dev-pc` (172.22.0.20): A high-privilege workstation with access to servers.
4.  **Server LAN (172.23.0.0/24)**:
    *   `internal-intranet` (172.23.0.10): Internal documentation site (Hints).
    *   `database` (172.23.0.20): PostgreSQL storing customer data.
    *   `backup-server` (172.23.0.30): The ultimate target.

## The Mission (Attack Path)

You play the role of the **Cobalt Group**. Your goal is to encrypt the **Database** and the **Backup Server**.

1.  **Initial Access**:
    *   Start by shelling into the HR PC: `docker exec -it hr-pc /bin/bash`
    *   You will find a "Phishing Simulation" script (`simulate_phish.py`) or just a `sticky_note.txt` with credentials.
    *   This represents the successful compromise of the first machine.

2.  **Discovery & Lateral Movement**:
    *   Explore the HR PC. Look for credentials or hints pointing to other internal assets.
    *   **Hint**: Check `sticky_note.txt` for credentials to `dev-pc`.
    *   Use `ssh` to move laterally to `dev-pc` (IP: 172.22.0.20).

3.  **Privilege Escalation / Access**:
    *   Once on `dev-pc`, look for information about the `Server LAN`.
    *   The `internal-intranet` (172.23.0.10) might have connection strings or passwords.
    *   The `dev-pc` is allowed to route to the Server LAN.

4.  **Action on Objectives**:
    *   Connect to the Database (Postgres) and "exfiltrate" (read) data.
    *   Connect to the Backup Server (SSH/SCP).
    *   "Deploy Ransomware" (e.g., use `openssl` or the provided `update.exe` if you can transfer it) to encrypt files on the Backup Server.

## commands

*   **Start**: `./start_range.sh`
*   **Stop**: `docker-compose down`
*   **Reset**: `docker-compose down -v`

## Tools Included
*   `nmap`: For scanning the network.
*   `ssh`: For remote access.
*   `curl/wget`: For downloading payloads.
*   `psql`: For database interaction.