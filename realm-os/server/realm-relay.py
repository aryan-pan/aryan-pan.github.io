#!/usr/bin/env python3
"""Realm relay: forwards every packet from one installation to all the others.

Installations on the same network find each other by broadcast without this.
Run it somewhere reachable and set `server=host:4777` in /etc/realm/realm.conf
to connect installations across the internet.
"""
import argparse
import socket
import time

EXPIRY = 15.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=4777)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    clients = {}
    print(f"realm relay listening on {args.host}:{args.port}", flush=True)
    while True:
        data, addr = sock.recvfrom(65535)
        now = time.monotonic()
        if addr not in clients:
            print(f"install joined from {addr[0]}:{addr[1]}", flush=True)
        clients[addr] = now
        for other, seen in list(clients.items()):
            if now - seen > EXPIRY:
                del clients[other]
            elif other != addr:
                sock.sendto(data, other)


if __name__ == "__main__":
    main()
