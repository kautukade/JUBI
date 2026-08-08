-- TechCorp Industries Database Initialization
-- Database: techcorp_production

USE techcorp_production;

-- Users table (for web authentication)
CREATE TABLE IF NOT EXISTS users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(255) NOT NULL,
    email VARCHAR(100),
    role VARCHAR(20) DEFAULT 'user',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert vulnerable users (plain text passwords for SQL injection practice)
INSERT INTO users (username, password, email, role) VALUES
('admin', 'Admin123!', 'admin@techcorp.local', 'admin'),
('developer', 'Dev2024!', 'developer@techcorp.local', 'developer'),
('marketing', 'Marketing2024!', 'marketing@techcorp.local', 'user'),
('support', 'Welcome123', 'support@techcorp.local', 'support');

-- Customers table
CREATE TABLE IF NOT EXISTS customers (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) NOT NULL,
    company VARCHAR(100),
    phone VARCHAR(20),
    address TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert sample customer data
INSERT INTO customers (name, email, company, phone, address) VALUES
('Acme Corporation', 'contact@acme.com', 'Acme Corp', '555-1000', '123 Business St, Tech City'),
('Global Industries', 'info@global.com', 'Global Inc', '555-2000', '456 Commerce Ave, Metro City'),
('Tech Solutions Ltd', 'hello@techsol.com', 'Tech Solutions', '555-3000', '789 Innovation Dr, Silicon Valley'),
('Enterprise Systems', 'sales@enterprise.com', 'Enterprise', '555-4000', '321 Corporate Blvd, Business Park'),
('Digital Dynamics', 'contact@digital.com', 'Digital Dynamics', '555-5000', '654 Tech Lane, Startup City');

-- Secrets table (sensitive data)
CREATE TABLE IF NOT EXISTS secrets (
    id INT AUTO_INCREMENT PRIMARY KEY,
    secret_key VARCHAR(255) NOT NULL,
    secret_value TEXT NOT NULL,
    description TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert sensitive secrets and flags
INSERT INTO secrets (secret_key, secret_value, description) VALUES
('FLAG_DATABASE', 'FLAG{d4t4b4s3_br34ch_c0mpl3t3}', 'Database breach flag'),
('API_KEY', 'sk_live_4eC39HqLyjWDarjtT1zdp7dc', 'Production API key'),
('AWS_ACCESS_KEY', 'AKIAIOSFODNN7EXAMPLE', 'AWS access key'),
('AWS_SECRET_KEY', 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY', 'AWS secret key'),
('STRIPE_KEY', 'sk_live_51HxBxKJ3xR4vXmPq8kEy2Zn', 'Stripe payment key'),
('JWT_SECRET', 'sup3r_s3cr3t_jwt_k3y_d0_n0t_sh4r3', 'JWT signing secret'),
('ENCRYPTION_KEY', 'aes256-encryption-key-32chars!!', 'Database encryption key'),
('ADMIN_PASSWORD_HASH', '$2y$10$92IXUNpkjO0rOQ5byMi.Ye4oKoEa3Ro9llC/.og/at2.uheWG/igi', 'Admin bcrypt hash');

-- Credit cards table (PCI compliance nightmare!)
CREATE TABLE IF NOT EXISTS credit_cards (
    id INT AUTO_INCREMENT PRIMARY KEY,
    customer_id INT,
    card_number VARCHAR(16) NOT NULL,
    cardholder_name VARCHAR(100) NOT NULL,
    expiry_date VARCHAR(7) NOT NULL,
    cvv VARCHAR(4) NOT NULL,
    billing_address TEXT,
    FOREIGN KEY (customer_id) REFERENCES customers(id)
);

-- Insert credit card data (NEVER store like this in production!)
INSERT INTO credit_cards (customer_id, card_number, cardholder_name, expiry_date, cvv, billing_address) VALUES
(1, '4532123456789012', 'John Acme', '12/2025', '123', '123 Business St, Tech City'),
(2, '5425233430109903', 'Jane Global', '06/2026', '456', '456 Commerce Ave, Metro City'),
(3, '340123456789019', 'Bob Tech', '03/2025', '789', '789 Innovation Dr, Silicon Valley'),
(4, '6011123456789012', 'Alice Enterprise', '09/2027', '321', '321 Corporate Blvd, Business Park'),
(5, '3566123456789016', 'Charlie Digital', '11/2024', '654', '654 Tech Lane, Startup City');

-- Employees table
CREATE TABLE IF NOT EXISTS employees (
    id INT AUTO_INCREMENT PRIMARY KEY,
    employee_id VARCHAR(10) NOT NULL UNIQUE,
    first_name VARCHAR(50) NOT NULL,
    last_name VARCHAR(50) NOT NULL,
    email VARCHAR(100) NOT NULL UNIQUE,
    department VARCHAR(50),
    position VARCHAR(50),
    salary DECIMAL(10, 2),
    ssn VARCHAR(11),
    hire_date DATE,
    manager_id INT
);

-- Insert employee data
INSERT INTO employees (employee_id, first_name, last_name, email, department, position, salary, ssn, hire_date, manager_id) VALUES
('EMP001', 'John', 'Smith', 'john.smith@techcorp.local', 'Engineering', 'Senior Developer', 125000.00, '123-45-6789', '2020-01-15', NULL),
('EMP002', 'Sarah', 'Johnson', 'sarah.johnson@techcorp.local', 'Marketing', 'Marketing Manager', 95000.00, '234-56-7890', '2019-06-01', NULL),
('EMP003', 'Michael', 'Chen', 'michael.chen@techcorp.local', 'IT', 'IT Administrator', 110000.00, '345-67-8901', '2018-03-20', NULL),
('EMP004', 'Emily', 'Davis', 'emily.davis@techcorp.local', 'Sales', 'Sales Director', 135000.00, '456-78-9012', '2017-09-10', NULL),
('EMP005', 'David', 'Wilson', 'david.wilson@techcorp.local', 'Engineering', 'DevOps Engineer', 115000.00, '567-89-0123', '2021-02-28', 1),
('EMP006', 'Admin', 'User', 'admin@techcorp.local', 'IT', 'System Administrator', 120000.00, '678-90-1234', '2016-05-15', NULL),
('EMP007', 'Dev', 'Account', 'developer@techcorp.local', 'Engineering', 'Developer', 105000.00, '789-01-2345', '2022-01-10', 1),
('EMP008', 'Support', 'Team', 'support@techcorp.local', 'Support', 'Support Engineer', 75000.00, '890-12-3456', '2023-07-01', NULL);

-- API Keys table
CREATE TABLE IF NOT EXISTS api_keys (
    id INT AUTO_INCREMENT PRIMARY KEY,
    key_name VARCHAR(100) NOT NULL,
    api_key VARCHAR(255) NOT NULL,
    api_secret VARCHAR(255),
    service VARCHAR(50),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_used TIMESTAMP NULL
);

-- Insert API keys
INSERT INTO api_keys (key_name, api_key, api_secret, service) VALUES
('GitHub API', 'ghp_1234567890abcdefghijklmnopqrstuvwxyz', NULL, 'GitHub'),
('AWS Production', 'AKIAIOSFODNN7EXAMPLE', 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY', 'AWS'),
('Stripe Production', 'sk_live_51HxBxKJ3xR4vXmPq8kEy2Zn', 'whsec_1234567890abcdefghijklmnopqrstuvwx', 'Stripe'),
('SendGrid Email', 'SG.1234567890abcdefghijklmnopqrstuvwxyz', NULL, 'SendGrid'),
('Slack Webhook', 'xoxb-1234567890-1234567890123-abcdefghijklmnopqrstuvwx', NULL, 'Slack');

-- Audit logs table
CREATE TABLE IF NOT EXISTS audit_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT,
    action VARCHAR(100) NOT NULL,
    table_name VARCHAR(50),
    record_id INT,
    ip_address VARCHAR(45),
    user_agent TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert some audit logs
INSERT INTO audit_logs (user_id, action, table_name, record_id, ip_address) VALUES
(1, 'LOGIN', 'users', 1, '192.168.10.10'),
(1, 'VIEW', 'customers', 1, '192.168.10.10'),
(2, 'LOGIN', 'users', 2, '192.168.20.50'),
(3, 'DATABASE_BACKUP', 'all', NULL, '192.168.20.50'),
(1, 'VIEW', 'secrets', 1, '192.168.10.10');

-- Flag table for clear flag tracking
CREATE TABLE IF NOT EXISTS flags (
    id INT AUTO_INCREMENT PRIMARY KEY,
    flag_number INT NOT NULL,
    flag_value VARCHAR(255) NOT NULL,
    description TEXT,
    location VARCHAR(100)
);

-- Insert all flags
INSERT INTO flags (flag_number, flag_value, description, location) VALUES
(1, 'FLAG{dns_z0n3_tr4nsf3r_l34k}', 'DNS zone transfer vulnerability', 'DNS Server - 192.168.10.53'),
(2, 'FLAG{sql_inj3ct10n_byp4ss}', 'SQL injection authentication bypass', 'Web Server - 192.168.10.10'),
(3, 'FLAG{rc3_4nd_sh3ll_upl04d}', 'Remote code execution via file upload', 'Web Server - 192.168.10.10'),
(4, 'FLAG{c0nf1g_f1l3s_3xp0s3d}', 'Exposed configuration files', 'Web Server - config.php'),
(5, 'FLAG{ftp_b4ckup_f1l3s_st0l3n}', 'FTP backup files with credentials', 'FTP Server - 192.168.10.20'),
(6, 'FLAG{sm7p_us3r_3num3r4t10n}', 'SMTP user enumeration', 'Mail Server - 192.168.10.25'),
(7, 'FLAG{ld4p_4n0nym0us_b1nd}', 'LDAP anonymous bind enabled', 'LDAP Server - 192.168.20.30'),
(8, 'FLAG{d4t4b4s3_br34ch_c0mpl3t3}', 'Database breach and data exfiltration', 'Database Server - 192.168.20.40'),
(9, 'FLAG{pr1v_3sc_v14_sud0_m1sc0nf1g}', 'Privilege escalation via sudo', 'File Server - 192.168.20.50'),
(10, 'FLAG{s3ns1t1v3_d4t4_3xf1ltr4t10n}', 'Sensitive data access', 'File Server - 192.168.20.50'),
(11, 'FLAG{d0m41n_4dm1n_pwn3d}', 'Domain admin compromise', 'LDAP Server - 192.168.20.30'),
(12, 'FLAG{l0g_s3rv3r_n0_4uth}', 'Log server without authentication', 'Log Server - 192.168.20.60');

-- Create a view for easy flag retrieval
CREATE VIEW all_flags AS
SELECT flag_number, flag_value, description, location
FROM flags
ORDER BY flag_number;

-- Grant permissions to webapp user
GRANT ALL PRIVILEGES ON techcorp_production.* TO 'webapp'@'%';

-- Grant remote access to root user (from fileserver in backend network)
-- Root password is set via MYSQL_ROOT_PASSWORD environment variable
-- Allow root to connect from any host with the same password
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' IDENTIFIED BY 'Tc0rp_Db_R00t_S3cur3_2024!' WITH GRANT OPTION;

FLUSH PRIVILEGES;

-- Display initialization complete message
SELECT 'Database initialization complete!' AS status;
SELECT COUNT(*) AS total_flags FROM flags;
