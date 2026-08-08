#!/bin/sh

# Populate the shared volume if empty or always to reset?
# Let's reset it every time for the range to be reusable easily.
echo "Restoring financial data..."
cp -r /dummy_data/* /data/

echo "Starting File Server..."
# We use /data which is the shared volume
exec python -m http.server 8000 --directory /data
