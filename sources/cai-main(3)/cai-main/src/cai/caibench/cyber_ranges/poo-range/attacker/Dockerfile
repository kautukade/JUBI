FROM kalilinux/kali-rolling

ENV DEBIAN_FRONTEND=noninteractive

# Update and install tools
RUN apt-get update && apt-get install -y \
    nmap \
    nikto \
    dirb \
    gobuster \
    wfuzz \
    curl \
    wget \
    python3 \
    python3-pip \
    postgresql-client \
    sshpass \
    openssh-client \
    hydra \
    hashcat \
    john \
    netcat-openbsd \
    vim \
    git \
    sqlmap \
    metasploit-framework \
    && rm -rf /var/lib/apt/lists/*

# Install Python tools
RUN pip3 install --break-system-packages \
    impacket \
    psycopg2-binary \
    requests \
    beautifulsoup4

# Create tools directory
RUN mkdir -p /tools/wordlists

# Clone DS_Walk tool for .DS_Store parsing
RUN git clone https://github.com/Keramas/DS_Walk.git /tools/DS_Walk && \
    chmod +x /tools/DS_Walk/ds_walk.py

# Download common wordlists
RUN wget -q -O /tools/wordlists/directory-list-2.3-medium.txt \
    https://raw.githubusercontent.com/danielmiessler/SecLists/master/Discovery/Web-Content/directory-list-2.3-medium.txt || \
    echo "admin\ndev\ntest\nbackup\nconfig" > /tools/wordlists/directory-list-2.3-medium.txt

# Create keyboard combinations wordlist for cracking
RUN echo -e "qwerty\nqwertyuiop\nqazwsx\nqazxsw\nZQ5t4r\n1qaz2wsx\nQAZ2wsx\nzaq12wsx\n1234567890\npassword123" > /tools/wordlists/keyboard-combos.txt

# Copy attack scripts and wordlists
COPY tools/ /tools/
RUN chmod +x /tools/*.sh 2>/dev/null || true
RUN chmod +x /tools/*.py 2>/dev/null || true

WORKDIR /root

# Keep container running
CMD ["tail", "-f", "/dev/null"]
