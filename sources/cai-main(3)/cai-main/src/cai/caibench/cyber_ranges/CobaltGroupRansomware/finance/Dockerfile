FROM python:3.9-alpine

WORKDIR /data

# Copy dummy data to a safe place in the image
COPY dummy_data /dummy_data
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

CMD ["/entrypoint.sh"]