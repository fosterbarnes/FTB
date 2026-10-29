"""Send files and folders to the Receive screen on a 3DS (FTB Title Manager).

Usage:
    python ftbsend.py [--ip HOST[:PORT]] [--to /3ds/] [--save] [--replace] PATH...

Open Receive on the 3DS first. Without --ip, the script finds the 3DS on the local network and
asks which one to use if there are several. CIA, TIK, CETK, and 3DSX files are installed unless
--save is given; other files are saved to the --to folder on the SD card. A folder is sent under
its own name, so dropping "saves" sends "saves/a.sav".
"""

import argparse
import http.client
import json
import os
import socket
import sys
import time
from dataclasses import dataclass

DISCOVERY_PORT = 5000
DISCOVERY_MESSAGE = b"ftbfind"
DISCOVERY_REPLY = b"ftbhere "
DISCOVERY_TRIES = 3
DISCOVERY_WAIT = 1.0
BROADCAST_ADDRESS = "255.255.255.255"
INSTALL_EXTENSIONS = (".cia", ".tik", ".cetk", ".3dsx")
MIN_INSTALL_SIZE = 4
MAX_FILE_SIZE = 0xFFFFFFFF  # FAT32 limit
CHUNK_SIZE = 256 * 1024
MIB = 1024 * 1024
PROGRESS_WIDTH = 100


@dataclass(frozen=True)
class Item:
    path: str  # manifest path, "/" separated
    local: str  # file on this PC
    size: int
    install: bool


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def host_header(host, port):
    return host if port == 80 else f"{host}:{port}"


def parse_host(value):
    host, sep, port = value.partition(":")
    if not sep:
        return host, 80
    if not (port.isdigit() and 1 <= int(port) <= 65535):
        raise argparse.ArgumentTypeError(f"invalid port: {port!r}")
    return host, int(port)


def parse_dest(value):
    if not value.startswith("/"):
        raise argparse.ArgumentTypeError("must start with /")
    return value if value.endswith("/") else value + "/"


def parse_args():
    parser = argparse.ArgumentParser(description="Send files or folders to the Receive screen on a 3DS.")
    parser.add_argument("--ip", type=parse_host, metavar="HOST[:PORT]",
                        help="3DS address (port defaults to 80). Skips searching the network.")
    parser.add_argument("--to", type=parse_dest, default="/", metavar="FOLDER",
                        help="SD folder for saved files (default: /).")
    parser.add_argument("--save", action="store_true",
                        help="save CIA/TIK/CETK/3DSX files to SD instead of installing them")
    parser.add_argument("--replace", action="store_true",
                        help="overwrite saved files that already exist")
    parser.add_argument("paths", nargs="+", metavar="PATH", help="files or folders to send")
    return parser.parse_args()


def parse_reply(data):
    digits = data[len(DISCOVERY_REPLY):]
    if not data.startswith(DISCOVERY_REPLY) or not digits.isdigit():
        return None
    port = int(digits)
    return port if 1 <= port <= 65535 else None


def collect_replies(sock, found):
    deadline = time.monotonic() + DISCOVERY_WAIT
    while (left := deadline - time.monotonic()) > 0:
        sock.settimeout(left)
        try:
            data, (ip, _) = sock.recvfrom(64)
        except socket.timeout:
            return
        port = parse_reply(data)
        if port:
            found.add((ip, port))


def discover():
    found = set()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        for _ in range(DISCOVERY_TRIES):
            sock.sendto(DISCOVERY_MESSAGE, (BROADCAST_ADDRESS, DISCOVERY_PORT))
            collect_replies(sock, found)
    return sorted(found)


def choose_target(found):
    if not found:
        fail("No 3DS found. Open Receive on the 3DS, or pass --ip.")
    if len(found) == 1:
        return found[0]
    print("Found more than one 3DS:")
    for number, (ip, port) in enumerate(found, 1):
        print(f"  {number}. {host_header(ip, port)}")
    while True:
        answer = input("Send to which one? ").strip()
        if answer.isdigit() and 1 <= int(answer) <= len(found):
            return found[int(answer) - 1]


def walk_folder(folder):
    base = os.path.basename(os.path.abspath(folder))
    for root, dirs, files in os.walk(folder):
        dirs[:] = sorted(name for name in dirs if not name.startswith("."))
        for name in sorted(n for n in files if not n.startswith(".")):
            local = os.path.join(root, name)
            relative = os.path.relpath(local, folder).replace(os.sep, "/")
            yield f"{base}/{relative}", local


def collect_sources(paths):
    sources = []
    for path in paths:
        if os.path.isdir(path):
            sources.extend(walk_folder(path))
        elif os.path.isfile(path):
            sources.append((os.path.basename(path), path))
        else:
            fail(f"Not a file or folder: {path}")
    return sources


def build_items(paths, save):
    items = []
    names = set()
    for manifest, local in collect_sources(paths):
        size = os.path.getsize(local)
        if size > MAX_FILE_SIZE:
            fail(f"Too big for FAT32 (over 4 GiB - 1 byte): {manifest}")
        install = not save and os.path.splitext(manifest)[1].lower() in INSTALL_EXTENSIONS
        if install and size < MIN_INSTALL_SIZE:
            fail(f"Too small to install (under 4 bytes): {manifest}")
        if manifest in names:
            fail(f"Duplicate path: {manifest}")
        names.add(manifest)
        items.append(Item(manifest, local, size, install))
    if not items:
        fail("No files to send.")
    return items


def read_json(resp):
    try:
        return json.loads(resp.read().decode("utf-8"))
    except ValueError:
        return {}


def check_status(resp, reply):
    if resp.status != 200:
        fail(f"{reply.get('error', 'Unexpected reply')} (HTTP {resp.status})")


def offer(host, port, dest, replace, items):
    body = json.dumps({
        "dest": dest,
        "replace": replace,
        "files": [{"path": i.path, "size": i.size, "install": i.install} for i in items],
    }).encode("utf-8")
    headers = {"X-FTB": "1", "Content-Type": "application/json", "Host": host_header(host, port)}
    conn = http.client.HTTPConnection(host, port, timeout=None)
    try:
        conn.request("POST", "/offer", body=body, headers=headers)
        print("Accept on the 3DS...")
        resp = conn.getresponse()
        reply = read_json(resp)
    except (OSError, http.client.HTTPException):
        fail(f"No reply from the 3DS at {host_header(host, port)}. Check the address and that Receive is open.")
    finally:
        conn.close()
    check_status(resp, reply)
    if not reply.get("accepted"):
        fail("Declined on the 3DS.")
    return reply["token"]


def file_chunks(item):
    remaining = item.size
    try:
        with open(item.local, "rb") as source:
            while remaining:
                chunk = source.read(min(CHUNK_SIZE, remaining))
                if not chunk:
                    fail(f"File got shorter while sending: {item.path}")
                remaining -= len(chunk)
                yield chunk
    except OSError as error:
        fail(f"Could not read {item.path}: {error}")


def print_progress(sent, total, started, item):
    elapsed = max(time.monotonic() - started, 0.001)
    line = (f"{sent * 100 // total:3d}%  {sent / MIB:.1f} / {total / MIB:.1f} MiB  "
            f"{sent / MIB / elapsed:.1f} MiB/s  {os.path.basename(item.path)}")
    sys.stdout.write("\r" + line.ljust(PROGRESS_WIDTH))
    sys.stdout.flush()


def stream_files(conn, items, total):
    sent = 0
    started = time.monotonic()
    try:
        for item in items:
            for chunk in file_chunks(item):
                conn.send(chunk)
                sent += len(chunk)
                print_progress(sent, total, started, item)
    finally:
        print()


def upload(host, port, token, items):
    total = sum(item.size for item in items)
    conn = http.client.HTTPConnection(host, port, timeout=None)
    try:
        conn.putrequest("POST", f"/data?token={token}", skip_host=True)
        conn.putheader("X-FTB", "1")
        conn.putheader("Host", host_header(host, port))
        conn.putheader("Content-Length", str(total))
        conn.endheaders()
        stream_files(conn, items, total)
        resp = conn.getresponse()
        reply = read_json(resp)
    except (OSError, http.client.HTTPException):
        fail("Stopped on the 3DS or connection lost.")
    finally:
        conn.close()
    check_status(resp, reply)
    results = reply.get("results", [])
    if len(results) != len(items):
        fail("Unexpected reply from the 3DS.")
    return results


def describe(item, result):
    if result.get("ok"):
        return f"{'Installed' if item.install else 'Saved'}: {item.path}"
    if result.get("skipped"):
        return f"Skipped ({result.get('error')}): {item.path}"
    return f"Failed: {item.path}: {result.get('error')}"


def report(items, results):
    for item, result in zip(items, results):
        print(describe(item, result))
    return all(result.get("ok") for result in results)


def main():
    args = parse_args()
    items = build_items(args.paths, args.save)
    if args.ip:
        host, port = args.ip
    else:
        print("Looking for a 3DS on the network...")
        host, port = choose_target(discover())
    print(f"Sending to {host_header(host, port)}.")
    token = offer(host, port, args.to, args.replace, items)
    results = upload(host, port, token, items)
    sys.exit(0 if report(items, results) else 1)


if __name__ == "__main__":
    main()
