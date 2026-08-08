FROM nginx:alpine

# Install required packages
RUN apk add --no-cache \
    bash \
    curl \
    openssl

# Create web directories
RUN mkdir -p /var/www/html/admin \
    /var/www/html/dev/304c0c90fbc6520610abbf378e2339d1/db \
    /var/www/html/dev/304c0c90fbc6520610abbf378e2339d1/core \
    /var/www/html/dev/dca66d38fd916317687e1390a420c3fc/db \
    /var/www/html/dev/dca66d38fd916317687e1390a420c3fc/core \
    /var/www/html/Images

# Copy configuration files
COPY config/nginx.conf /etc/nginx/nginx.conf
COPY config/default.conf /etc/nginx/conf.d/default.conf
COPY config/.htpasswd /etc/nginx/.htpasswd

# Copy web content
COPY www/ /var/www/html/

# Set permissions
RUN chown -R nginx:nginx /var/www/html && \
    chmod -R 755 /var/www/html && \
    chmod 644 /var/www/html/.DS_Store 2>/dev/null || true && \
    chown nginx:nginx /etc/nginx/.htpasswd && \
    chmod 644 /etc/nginx/.htpasswd

# Expose port
EXPOSE 80

# Start nginx
CMD ["nginx", "-g", "daemon off;"]
