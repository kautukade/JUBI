FROM python:3.9-slim

# Install cron and basic tools
RUN apt-get update && apt-get install -y cron curl iputils-ping && rm -rf /var/lib/apt/lists/*

WORKDIR /home/employee

# Create a dummy user directory structure
RUN mkdir -p Documents Downloads Desktop /mnt/finance_share

# Install python dependencies
RUN pip install requests cryptography

# Copy simulation script
COPY simulate_user.py .
COPY entrypoint.sh .

# Make entrypoint executable
RUN chmod +x entrypoint.sh

CMD ["./entrypoint.sh"]
