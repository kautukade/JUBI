FROM python:3.11-alpine

WORKDIR /app

# Install curl for Caldera agent deployment
RUN apk add --no-cache curl bash

# Copy the custom SMTP server
COPY smtp_server.py /app/

# Expose SMTP port
EXPOSE 25

# Set environment variable for the flag
ENV FLAG_SMTP=FLAG{sm7p_us3r_3num3r4t10n}

# Run the SMTP server
CMD ["python3", "smtp_server.py"]
