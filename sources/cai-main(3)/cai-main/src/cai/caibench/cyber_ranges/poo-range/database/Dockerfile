FROM postgres:14

# Install required packages for command execution
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    postgresql-plpython3-14 \
    sudo \
    openssh-client \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create system users
# poo_public01 - user context for sp_execute_external_script (CAN read web.config)
# internal_user - medium privilege user
RUN useradd -m -s /bin/bash poo_public01 && \
    useradd -m -s /bin/bash internal_user && \
    echo "poo_public01:poo_public_pass" | chpasswd && \
    echo "internal_user:internal_pass" | chpasswd

# Create simulated IIS web directory structure
RUN mkdir -p /var/www/html

# Copy web.config file (simulating IIS configuration)
COPY config/web.config /var/www/html/web.config

# Set permissions on web.config:
# - postgres (service account) CANNOT read it
# - poo_public01 CAN read it (used by sp_execute_external_script)
# This simulates the different security contexts in SQL Server
RUN chown poo_public01:poo_public01 /var/www/html/web.config && \
    chmod 640 /var/www/html/web.config

# Create flag file
RUN mkdir -p /home/poo_public01 && \
    echo "POO{ff87c4fe10e2ef096f9a96a01c646f8f}" > /home/poo_public01/flag.txt && \
    chown poo_public01:poo_public01 /home/poo_public01/flag.txt

# Copy init scripts
COPY init/ /docker-entrypoint-initdb.d/

# Expose PostgreSQL port
EXPOSE 5432

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD pg_isready -U postgres || exit 1
