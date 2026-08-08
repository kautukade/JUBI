#!/usr/bin/env python3
"""
Simple SMTP Server with VRFY support for Cyberrange Lab
Allows user enumeration via VRFY command for educational purposes
"""

import socket
import os

# Valid users in the system (only users enumerable via VRFY)
# dbadmin is the only enumerable user - this is the hidden account to discover
VALID_USERS = ['dbadmin']

def handle_client(conn, addr):
    """Handle SMTP client connection"""
    print(f"[*] Connection from {addr}")

    try:
        # Send banner
        conn.send(b"220 mail-server ESMTP\r\n")

        while True:
            data = conn.recv(1024)
            if not data:
                break

            command = data.decode('utf-8', errors='ignore').strip()
            print(f"[>] Received: {command}")

            # Parse command
            cmd_parts = command.split()
            if not cmd_parts:
                continue

            cmd = cmd_parts[0].upper()

            if cmd == "HELO" or cmd == "EHLO":
                conn.send(b"250 mail-server Hello\r\n")

            elif cmd == "VRFY":
                if len(cmd_parts) < 2:
                    conn.send(b"501 Syntax: VRFY <username>\r\n")
                else:
                    username = cmd_parts[1].lower()
                    if username in VALID_USERS:
                        response = f"250 {username}@techcorp.local\r\n".encode()
                        conn.send(response)
                    else:
                        response = f"550 {username}: User unknown\r\n".encode()
                        conn.send(response)

            elif cmd == "EXPN":
                conn.send(b"550 Access denied\r\n")

            elif cmd == "MAIL":
                conn.send(b"250 OK\r\n")

            elif cmd == "RCPT":
                conn.send(b"250 OK\r\n")

            elif cmd == "DATA":
                conn.send(b"354 End data with <CR><LF>.<CR><LF>\r\n")

            elif cmd == "QUIT":
                conn.send(b"221 Bye\r\n")
                break

            elif cmd == "RSET":
                conn.send(b"250 OK\r\n")

            elif cmd == "NOOP":
                conn.send(b"250 OK\r\n")

            else:
                conn.send(b"500 Command not recognized\r\n")

    except Exception as e:
        print(f"[!] Error handling client {addr}: {e}")
    finally:
        conn.close()
        print(f"[*] Connection closed from {addr}")


def main():
    """Main SMTP server loop"""
    host = '0.0.0.0'
    port = 25

    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((host, port))
    server_socket.listen(5)

    print(f"[*] SMTP Server listening on {host}:{port}")
    print(f"[*] Valid users: {', '.join(VALID_USERS)}")
    print(f"[*] FLAG: {os.getenv('FLAG_SMTP', 'FLAG not set')}")

    try:
        while True:
            conn, addr = server_socket.accept()
            handle_client(conn, addr)
    except KeyboardInterrupt:
        print("\n[!] Server shutting down...")
    finally:
        server_socket.close()


if __name__ == "__main__":
    main()
