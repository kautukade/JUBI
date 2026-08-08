-- POO Cyber Range Database Setup
-- Simulating MS SQL Server linked servers with PostgreSQL FDW

-- Enable required extensions
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE EXTENSION IF NOT EXISTS postgres_fdw;
CREATE EXTENSION IF NOT EXISTS plpython3u;

-- Create databases
CREATE DATABASE poo_public;
CREATE DATABASE poo_config;
CREATE DATABASE flag;

-- Create users with different privilege levels
-- external_user: Low privilege user (entry point)
CREATE USER external_user WITH PASSWORD '#p00Public3xt3rnalUs3r#';
-- internal_user: Medium privilege user (accessed via link)
CREATE USER internal_user WITH PASSWORD 'internal_secret_pass!';
-- super: High privilege user (created through escalation)

-- Grant basic permissions
GRANT CONNECT ON DATABASE poo_public TO external_user;
GRANT CONNECT ON DATABASE poo_config TO internal_user;

\c poo_public

-- Enable extensions in poo_public
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE EXTENSION IF NOT EXISTS postgres_fdw;

-- Create schema and tables
CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    username VARCHAR(50),
    password VARCHAR(100),
    role VARCHAR(20)
);

INSERT INTO users (username, password, role) VALUES
    ('admin', 'admin_secret_hash', 'admin'),
    ('guest', 'guest_pass', 'user');

-- Create syslogins table (similar to SQL Server master.dbo.syslogins)
-- This shows database users and their privilege levels
CREATE TABLE syslogins (
    name VARCHAR(50),
    sysadmin INTEGER,  -- 1 = sysadmin, 0 = not sysadmin
    createdate TIMESTAMP DEFAULT NOW()
);

INSERT INTO syslogins (name, sysadmin) VALUES
    ('postgres', 1),           -- superuser (sa equivalent)
    ('external_user', 0),      -- low privilege user
    ('internal_user', 0);      -- medium privilege user

GRANT SELECT ON syslogins TO external_user;

-- Create sysservers table (similar to SQL Server master.dbo.sysservers)
-- This shows linked servers configuration
CREATE TABLE sysservers (
    srvid INTEGER,
    srvname VARCHAR(100),
    srvproduct VARCHAR(50),
    providername VARCHAR(50),
    datasource VARCHAR(100)
);

INSERT INTO sysservers (srvid, srvname, srvproduct, providername, datasource) VALUES
    (0, 'POO_PUBLIC', 'PostgreSQL', 'postgres_fdw', 'localhost'),
    (1, 'POO_CONFIG', 'PostgreSQL', 'postgres_fdw', 'poo_config');

GRANT SELECT ON sysservers TO external_user;

-- Create linked server to poo_config (simulating SQL Server linked servers)
CREATE SERVER poo_config_link
    FOREIGN DATA WRAPPER postgres_fdw
    OPTIONS (host 'localhost', dbname 'poo_config', port '5432');

-- User mapping - external_user connects to poo_config as internal_user
CREATE USER MAPPING FOR external_user
    SERVER poo_config_link
    OPTIONS (user 'internal_user', password 'internal_secret_pass!');

GRANT USAGE ON FOREIGN SERVER poo_config_link TO external_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO external_user;
GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO external_user;

-- Create function for remote query execution (similar to EXEC at linked server)
CREATE OR REPLACE FUNCTION exec_on_config(query TEXT)
RETURNS TABLE(result TEXT) AS $$
BEGIN
    RETURN QUERY SELECT * FROM dblink(
        'dbname=poo_config user=internal_user password=internal_secret_pass!',
        query
    ) AS t(result TEXT);
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

GRANT EXECUTE ON FUNCTION exec_on_config(TEXT) TO external_user;

\c poo_config

-- Enable extensions in poo_config
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE EXTENSION IF NOT EXISTS postgres_fdw;
CREATE EXTENSION IF NOT EXISTS plpython3u;

-- Create linked server back to poo_public (circular link!)
CREATE SERVER poo_public_link
    FOREIGN DATA WRAPPER postgres_fdw
    OPTIONS (host 'localhost', dbname 'poo_public', port '5432');

-- Critical vulnerability: internal_user connects back as postgres (superuser)!
CREATE USER MAPPING FOR internal_user
    SERVER poo_public_link
    OPTIONS (user 'postgres', password 'SuperSecretDBRoot!');

GRANT USAGE ON FOREIGN SERVER poo_public_link TO internal_user;

-- Create config table with sensitive info (but NOT admin password - must be found via file read)
CREATE TABLE config (
    key VARCHAR(50),
    value TEXT
);

INSERT INTO config (key, value) VALUES
    ('db_version', '14.0'),
    ('admin_email', 'admin@intranet.poo'),
    ('admin_user', 'Administrator'),
    ('admin_password_hint', 'Check IIS web.config file at /var/www/html/web.config');

-- Create sysservers table showing linked servers from poo_config perspective
-- This reveals the circular link back to poo_public!
CREATE TABLE sysservers (
    srvid INTEGER,
    srvname VARCHAR(100),
    srvproduct VARCHAR(50),
    providername VARCHAR(50),
    datasource VARCHAR(100)
);

INSERT INTO sysservers (srvid, srvname, srvproduct, providername, datasource) VALUES
    (0, 'POO_CONFIG', 'PostgreSQL', 'postgres_fdw', 'localhost'),
    (1, 'POO_PUBLIC', 'PostgreSQL', 'postgres_fdw', 'poo_public');

GRANT SELECT ON sysservers TO internal_user;

-- Function to execute on poo_public as postgres (privilege escalation!)
CREATE OR REPLACE FUNCTION exec_on_public_as_sa(query TEXT)
RETURNS TABLE(result TEXT) AS $$
BEGIN
    RETURN QUERY SELECT * FROM dblink(
        'dbname=poo_public user=postgres password=SuperSecretDBRoot!',
        query
    ) AS t(result TEXT);
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

GRANT EXECUTE ON FUNCTION exec_on_public_as_sa(TEXT) TO internal_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO internal_user;

\c flag

-- Flag database
CREATE TABLE flag (
    flag TEXT
);

INSERT INTO flag VALUES ('POO{88d829eb39f2d11697e689d779810d42}');

-- Grant select to postgres only (need privesc to read)
GRANT SELECT ON flag TO postgres;

\c poo_public

-- ============================================================================
-- SQL Server Stored Procedure Emulation for Linux/PostgreSQL
-- These emulate real SQL Server procedures using PL/Python
-- ============================================================================

-- Enable plpython3u extension (untrusted - allows OS access)
CREATE EXTENSION IF NOT EXISTS plpython3u;

-- ============================================================================
-- xp_cmdshell - SQL Server's OS command execution procedure
-- https://docs.microsoft.com/en-us/sql/relational-databases/system-stored-procedures/xp-cmdshell-transact-sql
-- Runs as the SQL Server service account (in our case: postgres)
-- ============================================================================
CREATE OR REPLACE FUNCTION xp_cmdshell(cmd TEXT)
RETURNS TABLE(output TEXT) AS $$
import subprocess
result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
output = (result.stdout + result.stderr).strip()
return output.split('\n') if output else [None]
$$ LANGUAGE plpython3u;

GRANT EXECUTE ON FUNCTION xp_cmdshell(TEXT) TO external_user;

-- ============================================================================
-- sp_execute_external_script - SQL Server's external script execution
-- https://docs.microsoft.com/en-us/sql/relational-databases/system-stored-procedures/sp-execute-external-script-transact-sql
-- In SQL Server, this runs R/Python in a separate process with different permissions
-- Key vulnerability: runs as a DIFFERENT user (poo_public01) than xp_cmdshell
-- ============================================================================
CREATE OR REPLACE FUNCTION sp_execute_external_script(
    language TEXT,
    script TEXT
)
RETURNS TABLE(output TEXT) AS $$
import subprocess
import pwd

# Validate language (SQL Server supports R and Python)
if language.upper() not in ('PYTHON', 'R'):
    raise Exception("Only @language = N'Python' or N'R' is supported")

# Get poo_public01 user - simulates SQL Server's external script launcher account
poo_user = pwd.getpwnam('poo_public01')

# Execute script as poo_public01 (DIFFERENT security context than xp_cmdshell!)
result = subprocess.run(
    ['python3', '-c', script],
    capture_output=True,
    text=True,
    user=poo_user.pw_uid,
    group=poo_user.pw_gid,
    cwd='/tmp'
)

output = (result.stdout + result.stderr).strip()
return output.split('\n') if output else ['(no output)']
$$ LANGUAGE plpython3u;

GRANT EXECUTE ON FUNCTION sp_execute_external_script(TEXT, TEXT) TO external_user;

\c postgres

-- Create Python command execution function for superuser
CREATE OR REPLACE FUNCTION run_command(cmd TEXT)
RETURNS TEXT AS $$
import subprocess
result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
return result.stdout + result.stderr
$$ LANGUAGE plpython3u;

-- Only superuser can execute initially
REVOKE EXECUTE ON FUNCTION run_command(TEXT) FROM PUBLIC;
