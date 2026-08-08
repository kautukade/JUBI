TechCorp File Server
====================

This server contains shared files for TechCorp employees.

Directory Structure:
--------------------
/data/shares/public - Public shared files
/data/shares/engineering - Engineering department files
/data/shares/executive - Executive documents (restricted)
/data/backups - Automated backups

Access:
-------
SSH: ssh developer@192.168.20.50 (password: Dev2024!)
SSH: ssh backupuser@192.168.20.50 (password: Backup123)

Sudo Privileges:
----------------
developer: Can run /usr/bin/rsync as root
backupuser: Can run /usr/bin/tar as root

For support, contact: support@techcorp.local
