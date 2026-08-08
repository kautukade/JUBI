FROM ubuntu:latest

RUN apt-get update && apt-get install -y \
    curl \
    python3 \
    python3-pip \
    ssh \
    openssh-server \
    vim \
    postgresql-client \
    nmap \
    && rm -rf /var/lib/apt/lists/*

# Setup SSH Server for lateral movement target
RUN mkdir /var/run/sshd
RUN echo 'root:toor' | chpasswd
RUN sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config

# Create Dev User
RUN useradd -m -s /bin/bash alice_dev
RUN echo 'alice_dev:developer123' | chpasswd

WORKDIR /home/alice_dev

# SSH Keys (to be generated or copied)
# We will generate them in the build or entrypoint
RUN mkdir .ssh && chown alice_dev:alice_dev .ssh

CMD ["/usr/sbin/sshd", "-D"]
