#!/bin/bash

# Start cron service
service cron start

# Run user simulation
python simulate_user.py
