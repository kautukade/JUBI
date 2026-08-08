FROM alpine:latest

RUN apk add --no-cache iptables iproute2

# Enable forwarding
RUN echo "net.ipv4.ip_forward=1" >> /etc/sysctl.conf

# We can add a startup script to configure NAT/Firewall rules
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

CMD ["/entrypoint.sh"]
