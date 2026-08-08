FROM ubuntu:20.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Fix GPG keys and install necessary tools
RUN rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/* && \
    apt-get clean && \
    apt-get update --allow-insecure-repositories && \
    apt-get install -y --allow-unauthenticated ca-certificates && \
    apt-get update && \
    apt-get install -y \
    openssh-server \
    python3 \
    python3-pip \
    curl \
    wget \
    vim \
    nano \
    net-tools \
    iputils-ping \
    netcat \
    cron \
    sudo \
    gcc \
    make \
    git \
    && rm -rf /var/lib/apt/lists/*

# Configure SSH
RUN mkdir /var/run/sshd
RUN sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config
RUN sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Create vulnerable user accounts
RUN useradd -m -s /bin/bash -G sudo webadmin && echo 'webadmin:WebAdmin123!' | chpasswd
RUN useradd -m -s /bin/bash dbuser && echo 'dbuser:Database2023' | chpasswd
RUN useradd -m -s /bin/bash jsmith && echo 'jsmith:Summer2023' | chpasswd

# Create sensitive files and directories
RUN mkdir -p /opt/company/config /opt/company/backups /opt/company/logs

# Add database configuration file with credentials
RUN echo "DB_HOST=prod-db.company.local" > /opt/company/config/database.conf && \
    echo "DB_USER=admin" >> /opt/company/config/database.conf && \
    echo "DB_PASSWORD=Pr0dDB_P@ssw0rd!" >> /opt/company/config/database.conf && \
    echo "DB_NAME=customer_data" >> /opt/company/config/database.conf && \
    echo "# FLAG{ph4s3_1_d1sc0v3ry_cr3d3nt14ls_f0und}" >> /opt/company/config/database.conf && \
    chmod 644 /opt/company/config/database.conf

# Add API configuration
RUN echo "API_KEY=sk_live_51KxYzABcD3fGhIjKlMn0pQrStUvWxYz" > /opt/company/config/api_keys.conf && \
    echo "AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE" >> /opt/company/config/api_keys.conf && \
    echo "AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY" >> /opt/company/config/api_keys.conf && \
    echo "# Congratulations on finding the API keys!" >> /opt/company/config/api_keys.conf && \
    chmod 600 /opt/company/config/api_keys.conf && \
    chown webadmin:webadmin /opt/company/config/api_keys.conf

# Add SSH keys for webadmin
RUN mkdir -p /home/webadmin/.ssh && \
    ssh-keygen -t rsa -b 2048 -f /home/webadmin/.ssh/id_rsa -N "" && \
    chown -R webadmin:webadmin /home/webadmin/.ssh && \
    chmod 700 /home/webadmin/.ssh && \
    chmod 600 /home/webadmin/.ssh/id_rsa

# Create customer data files
RUN echo "Customer Database Export - Q4 2023" > /opt/company/backups/customer_export.csv && \
    echo "id,name,email,credit_card" >> /opt/company/backups/customer_export.csv && \
    echo "1,John Doe,john.doe@email.com,4532-1234-5678-9012" >> /opt/company/backups/customer_export.csv && \
    echo "2,Jane Smith,jane.smith@email.com,5412-9876-5432-1098" >> /opt/company/backups/customer_export.csv && \
    echo "3,Bob Johnson,bob.j@email.com,3782-8224-6310-0055" >> /opt/company/backups/customer_export.csv && \
    echo "# FLAG{ph4s3_5_d4t4_3xf1ltr4t10n_c0mpl3t3}" >> /opt/company/backups/customer_export.csv && \
    chmod 644 /opt/company/backups/customer_export.csv

# Add financial reports
RUN echo "Q4 Financial Report - Confidential" > /opt/company/backups/financial_report.txt && \
    echo "Revenue: $2,450,000" >> /opt/company/backups/financial_report.txt && \
    echo "Operating Expenses: $1,200,000" >> /opt/company/backups/financial_report.txt && \
    echo "Net Profit: $1,250,000" >> /opt/company/backups/financial_report.txt && \
    chmod 644 /opt/company/backups/financial_report.txt

# Create user documents with credentials
RUN mkdir -p /home/webadmin/Documents && \
    echo "Server Access Notes" > /home/webadmin/Documents/notes.txt && \
    echo "Production Server: 10.0.1.50" >> /home/webadmin/Documents/notes.txt && \
    echo "Admin Portal: https://admin.company.local" >> /home/webadmin/Documents/notes.txt && \
    echo "Username: webadmin" >> /home/webadmin/Documents/notes.txt && \
    echo "Password: WebAdmin123!" >> /home/webadmin/Documents/notes.txt && \
    chown -R webadmin:webadmin /home/webadmin/Documents

# Add browser history simulation file
RUN mkdir -p /home/jsmith/Downloads && \
    echo "Employee Portal Login" > /home/jsmith/Downloads/bookmarks.txt && \
    echo "https://portal.company.local/login" >> /home/jsmith/Downloads/bookmarks.txt && \
    echo "Username: jsmith" >> /home/jsmith/Downloads/bookmarks.txt && \
    echo "Password: Summer2023" >> /home/jsmith/Downloads/bookmarks.txt && \
    chown -R jsmith:jsmith /home/jsmith/Downloads

# Create a "secure" directory with sudo-only access containing more sensitive data
RUN mkdir -p /root/secure && \
    echo "Root Password: R00tP@ss2023!" > /root/secure/credentials.txt && \
    echo "FLAG{ph4s3_4_syst3m_m0d1f1c4t10n_pr1v_3sc}" >> /root/secure/credentials.txt && \
    echo "VPN Key: -----BEGIN PRIVATE KEY-----" >> /root/secure/vpn_key.pem && \
    echo "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC..." >> /root/secure/vpn_key.pem && \
    chmod 600 /root/secure/*

# Add a simple web service simulation
RUN mkdir -p /var/www/html && \
    echo "<html><body><h1>Company Internal Portal</h1></body></html>" > /var/www/html/index.html

# Setup cron (for persistence simulation)
RUN service cron start

# Create a startup script
RUN echo '#!/bin/bash' > /startup.sh && \
    echo 'service ssh start' >> /startup.sh && \
    echo 'service cron start' >> /startup.sh && \
    echo 'echo "System Ready - Vulnerable Server Online"' >> /startup.sh && \
    echo 'echo "Users: webadmin, dbuser, jsmith"' >> /startup.sh && \
    echo 'echo "Services: SSH (port 22)"' >> /startup.sh && \
    echo 'tail -f /dev/null' >> /startup.sh && \
    chmod +x /startup.sh

EXPOSE 22

CMD ["/bin/bash", "/startup.sh"]
