FROM php:7.4-apache

# Install mysqli extension for database connectivity
RUN docker-php-ext-install mysqli pdo pdo_mysql

# Enable Apache modules
RUN a2enmod rewrite

# Copy web files
COPY ./html/ /var/www/html/

# Set permissions
RUN chown -R www-data:www-data /var/www/html \
    && chmod -R 755 /var/www/html

# Create uploads directory
RUN mkdir -p /var/www/html/uploads \
    && chown www-data:www-data /var/www/html/uploads \
    && chmod 777 /var/www/html/uploads

# Add users visible in /etc/passwd for reconnaissance
# SMTP enumerable users (will show in VRFY)
RUN useradd -m -s /bin/bash admin -c "System Administrator"
RUN useradd -m -s /bin/bash developer -c "Development Team"
RUN useradd -m -s /bin/bash support -c "Support Team"

# Hidden users (NOT in SMTP VRFY - interesting for attackers)
RUN useradd -m -s /bin/bash dbadmin -c "Database Administrator"
RUN useradd -m -s /bin/bash sysadmin -c "Systems Administrator"

# Note: These users exist in /etc/passwd but don't have passwords set on this container
# They DO have SSH access on the fileserver with weak passwords

EXPOSE 80

CMD ["apache2-foreground"]
