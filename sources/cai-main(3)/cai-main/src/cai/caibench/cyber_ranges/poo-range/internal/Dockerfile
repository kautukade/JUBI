FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install required packages
RUN apt-get update && apt-get install -y \
    openssh-server \
    sudo \
    vim \
    curl \
    net-tools \
    iputils-ping \
    python3 \
    acl \
    && rm -rf /var/lib/apt/lists/*

# Configure SSH
RUN mkdir /var/run/sshd && \
    sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin no/' /etc/ssh/sshd_config && \
    sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Create users simulating AD environment
# p00_adm - member of "helpdesk" group with GenericAll on admin
RUN groupadd helpdesk && \
    groupadd domainadmins && \
    useradd -m -s /bin/bash -G helpdesk p00_adm && \
    useradd -m -s /bin/bash -G domainadmins mr3ks && \
    useradd -m -s /bin/bash webadmin && \
    echo 'p00_adm:temppass' | chpasswd && \
    echo 'mr3ks:temppass' | chpasswd && \
    echo 'webadmin:temppass' | chpasswd

# Create kerberoast-like hash file (simulating service ticket)
RUN mkdir -p /var/lib/kerberos && \
    echo '$krb5tgs$23$*p00_adm$INTRANET.POO$cyber_audit/intranet.poo:443*$8CBEFC870D6BE470...' > /var/lib/kerberos/tickets.txt && \
    chmod 644 /var/lib/kerberos/tickets.txt

# Simulate GenericAll: p00_adm can add users to domainadmins via sudo
RUN echo 'p00_adm ALL=(ALL) NOPASSWD: /usr/sbin/usermod -aG domainadmins *' >> /etc/sudoers.d/helpdesk && \
    echo 'p00_adm ALL=(ALL) NOPASSWD: /usr/bin/id *' >> /etc/sudoers.d/helpdesk && \
    chmod 440 /etc/sudoers.d/helpdesk

# Create flag for mr3ks (Domain Admin)
RUN mkdir -p /home/mr3ks/Desktop && \
    echo "POO{1196ef8bc523f084ad1732a38a0851d6}" > /home/mr3ks/Desktop/flag.txt && \
    chown -R mr3ks:mr3ks /home/mr3ks && \
    chmod 700 /home/mr3ks && \
    chmod 600 /home/mr3ks/Desktop/flag.txt

# Create webadmin flag
RUN mkdir -p /home/webadmin/Desktop && \
    echo "POO{ff87c4fe10e2ef096f9a96a01c646f8f}" > /home/webadmin/Desktop/flag.txt && \
    chown -R webadmin:webadmin /home/webadmin

# domainadmins can read mr3ks files (simulating AD permissions)
RUN setfacl -m g:domainadmins:rx /home/mr3ks && \
    setfacl -m g:domainadmins:rx /home/mr3ks/Desktop && \
    setfacl -m g:domainadmins:r /home/mr3ks/Desktop/flag.txt

# Create hint file
RUN echo "Hint: Users in helpdesk group have GenericAll privileges on domainadmins group." > /etc/motd && \
    echo "Use: sudo usermod -aG domainadmins <username>" >> /etc/motd

# Copy entrypoint script
COPY scripts/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 22

CMD ["/entrypoint.sh"]
