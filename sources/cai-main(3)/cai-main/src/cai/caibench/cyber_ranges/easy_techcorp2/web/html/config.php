<?php
// FLAG 4: Configuration File Exposure
// This file should NOT be web-accessible!

// Database Configuration
define('DB_HOST', '192.168.20.40');
define('DB_USER', 'webapp');
define('DB_PASS', 'DBpass123');
define('DB_NAME', 'techcorp_production');
define('DB_ROOT_USER', 'root');
define('DB_ROOT_PASS', 'Tc0rp_Db_R00t_S3cur3_2024!');  // COMPLEX PASSWORD!

// FTP Configuration
define('FTP_HOST', '192.168.10.20');
define('FTP_USER', 'marketing');
define('FTP_PASS', 'Marketing2024!');
define('FTP_ANON_ENABLED', true);

// LDAP Configuration
define('LDAP_HOST', '192.168.20.30');
define('LDAP_PORT', 389);
define('LDAP_ADMIN_DN', 'cn=admin,dc=techcorp,dc=local');
define('LDAP_ADMIN_PASS', 'TechCorp2024');
define('LDAP_ANON_BIND', true);  // SECURITY ISSUE!

// File Server Configuration
define('FILE_SERVER_HOST', '192.168.20.50');
define('FILE_SERVER_USER', 'developer');
define('FILE_SERVER_PASS', 'Dev2024!');
define('FILE_SERVER_BACKUP_USER', 'backup');
define('FILE_SERVER_BACKUP_PASS', 'Backup123');

// Mail Server Configuration
define('MAIL_HOST', '192.168.10.25');
define('MAIL_PORT', 587);
define('MAIL_USER', 'support@techcorp.local');
define('MAIL_PASS', 'Welcome123');

// API Keys (SHOULD BE IN ENVIRONMENT VARIABLES!)
define('API_KEY', 'sk_live_4eC39HqLyjWDarjtT1zdp7dc');
define('SECRET_KEY', 'whsec_1234567890abcdefghijklmnopqrstuvwx');

// AWS Credentials (CRITICAL EXPOSURE!)
define('AWS_ACCESS_KEY', 'AKIAIOSFODNN7EXAMPLE');
define('AWS_SECRET_KEY', 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY');
define('AWS_REGION', 'us-east-1');
define('S3_BUCKET', 'techcorp-backups');

// Flag for finding this file
$flag = "FLAG{c0nf1g_f1l3s_3xp0s3d}";

echo "<h1>Configuration File</h1>";
echo "<p style='color: red; font-size: 20px;'>🚩 " . $flag . "</p>";
echo "<pre>";
echo "This file contains sensitive configuration data.\n";
echo "In a production environment, this should NEVER be web-accessible!\n\n";
echo "Found credentials for:\n";
echo "- Database (including root)\n";
echo "- FTP Server\n";
echo "- LDAP/Active Directory\n";
echo "- File Server\n";
echo "- Mail Server\n";
echo "- API Keys\n";
echo "- AWS Credentials\n";
echo "</pre>";
?>
