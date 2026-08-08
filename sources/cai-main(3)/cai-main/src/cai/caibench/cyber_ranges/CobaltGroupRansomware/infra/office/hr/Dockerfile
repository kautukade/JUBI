FROM ubuntu:latest

# Install tools for lateral movement and simulation
RUN apt-get update && apt-get install -y \
    curl \
    wget \
    python3 \
    python3-pip \
    net-tools \
    iputils-ping \
    ssh \
    vim \
    nano \
    nmap \
    python3-requests \
    && rm -rf /var/lib/apt/lists/*

# Create user
RUN useradd -m -s /bin/bash jdoe_hr

WORKDIR /home/jdoe_hr
USER jdoe_hr

# Add some dummy files
RUN echo "Reminder: Submit expense reports by Friday." > /home/jdoe_hr/todo.txt

# Add the "Phishing Email" (Simulation script will use this)
COPY --chown=jdoe_hr:jdoe_hr simulate_phish.py .
COPY --chown=jdoe_hr:jdoe_hr post_install.sh .
RUN sh post_install.sh

CMD ["tail", "-f", "/dev/null"]

