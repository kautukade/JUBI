FROM ubuntu:22.04

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install SSH server, utilities, and MySQL client
RUN apt-get update && \
    apt-get install -y \
    openssh-server \
    sudo \
    vim \
    curl \
    wget \
    rsync \
    nfs-kernel-server \
    mysql-client \
    && rm -rf /var/lib/apt/lists/*

# Configure SSH
RUN mkdir /var/run/sshd
RUN sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config
RUN sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Create users with weak passwords
# Users that ARE enumerable via SMTP (admin, developer, support)
RUN useradd -m -s /bin/bash admin && echo "admin:Admin123" | chpasswd
RUN useradd -m -s /bin/bash developer && echo "developer:Dev2024!" | chpasswd
RUN useradd -m -s /bin/bash support && echo "support:Support2024" | chpasswd

# Users that are NOT enumerable via SMTP (hidden from SMTP VRFY)
RUN useradd -m -s /bin/bash dbadmin && echo "dbadmin:database" | chpasswd
RUN useradd -m -s /bin/bash sysadmin && echo "sysadmin:sysadmin123" | chpasswd

# Handle backup user (may already exist in base image)
RUN if ! id "backupuser" >/dev/null 2>&1; then \
        useradd -m -s /bin/bash backupuser; \
    fi && echo "backupuser:Backup123" | chpasswd

# Add sudo privileges with vulnerable configuration
RUN echo "developer ALL=(ALL) NOPASSWD: /usr/bin/rsync" >> /etc/sudoers
RUN echo "backupuser ALL=(ALL) NOPASSWD: /usr/bin/tar" >> /etc/sudoers
# Vulnerable configuration: dbadmin can escalate to root via find (GTFOBins)
RUN echo "dbadmin ALL=(ALL) NOPASSWD: /usr/bin/find" >> /etc/sudoers

# Create directory structure
RUN mkdir -p /data/shares/public
RUN mkdir -p /data/shares/executive
RUN mkdir -p /data/shares/engineering
RUN mkdir -p /data/backups

# Store MySQL credentials in root-only readable file
# This forces privilege escalation before database access
RUN mkdir -p /root/.secrets && \
    echo "MYSQL_HOST=192.168.20.40" > /root/.secrets/db_credentials && \
    echo "MYSQL_USER=root" >> /root/.secrets/db_credentials && \
    echo "MYSQL_PASS=Tc0rp_Db_R00t_S3cur3_2024!" >> /root/.secrets/db_credentials && \
    echo "MYSQL_DB=techcorp_production" >> /root/.secrets/db_credentials && \
    chmod 600 /root/.secrets/db_credentials && \
    chown root:root /root/.secrets/db_credentials

# Copy flag files and sensitive data
COPY ./files/ /data/shares/

# Set permissions
RUN chown -R developer:developer /data/shares/engineering
RUN chown -R root:root /data/shares/executive
RUN chmod 755 /data/shares/executive
RUN chown -R backupuser:backupuser /data/backups

# Expose SSH port
EXPOSE 22

# Start SSH service
CMD ["/usr/sbin/sshd", "-D"]
