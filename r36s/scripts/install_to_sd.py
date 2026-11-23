#!/usr/bin/env python3
"""
Detects the Knulli/PortMaster SD card and installs the latest packaged build.
"""
import hashlib
import os
import shutil
import string
import sys

def get_drive_letters():
    return [f"{d}:\\" for d in string.ascii_uppercase if os.path.exists(f"{d}:\\")]

def find_target_port_dir():
    # Look for any drive with roms/ports or roms/ports/nfshp2
    candidates = []
    for d in get_drive_letters():
        p1 = os.path.join(d, "roms", "ports")
        if os.path.isdir(p1):
            candidates.append(p1)
        p2 = os.path.join(d, "ports")
        if os.path.isdir(p2):
            candidates.append(p2)
    return candidates

def hash_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def ensure_lf(path):
    if not os.path.isfile(path) or path.endswith((".jpg", ".png", ".armhf", "unpack")):
        return
    with open(path, "rb") as f:
        data = f.read()
    if b"\r\n" in data:
        data = data.replace(b"\r\n", b"\n")
        with open(path, "wb") as f:
            f.write(data)

def install(ports_dir, package_dir):
    nfshp2_dir = os.path.join(ports_dir, "nfshp2")
    os.makedirs(nfshp2_dir, exist_ok=True)
    
    # Copy root launcher
    src_sh = os.path.join(package_dir, "NFSHP2.sh")
    dst_sh = os.path.join(ports_dir, "NFSHP2.sh")
    if os.path.isfile(src_sh):
        shutil.copy2(src_sh, dst_sh)
        ensure_lf(dst_sh)
        print(f"Copied {dst_sh} ({os.path.getsize(dst_sh)} bytes)")

    # Copy files inside nfshp2
    for item in os.listdir(os.path.join(package_dir, "nfshp2")):
        src = os.path.join(package_dir, "nfshp2", item)
        dst = os.path.join(nfshp2_dir, item)
        if os.path.isdir(src):
            if item == "gamedata":
                # Do not overwrite user's gamedata, only ensure directory and README.txt
                os.makedirs(dst, exist_ok=True)
                src_txt = os.path.join(src, "README.txt")
                if os.path.isfile(src_txt):
                    shutil.copy2(src_txt, os.path.join(dst, "README.txt"))
                    ensure_lf(os.path.join(dst, "README.txt"))
            else:
                shutil.copytree(src, dst, dirs_exist_ok=True)
                for root, _, files in os.walk(dst):
                    for f in files:
                        ensure_lf(os.path.join(root, f))
                print(f"Updated tree {item}/")
        else:
            if item == "nfshp2.cfg" and os.path.isfile(dst):
                print("Preserving existing nfshp2.cfg")
                continue
            shutil.copy2(src, dst)
            ensure_lf(dst)
            print(f"Copied {item} ({os.path.getsize(dst)} bytes)")

    # Copy metadata files
    for meta in ["port.json", "gameinfo.xml", "README.md", "screenshot.jpg", "cover.jpg"]:
        src = os.path.join(package_dir, meta)
        dst = os.path.join(nfshp2_dir, meta)
        if os.path.isfile(src):
            shutil.copy2(src, dst)
            ensure_lf(dst)

    # Report verification
    armhf_bin = os.path.join(nfshp2_dir, "nfshp2.armhf")
    if os.path.isfile(armhf_bin):
        print(f"Installed binary: {armhf_bin}")
        print(f"  Size: {os.path.getsize(armhf_bin)} bytes")
        print(f"  SHA256: {hash_file(armhf_bin)}")

    unpack_bin = os.path.join(nfshp2_dir, "tools", "nfshp2-unpack")
    if os.path.isfile(unpack_bin):
        print(f"Installed unpacker: {unpack_bin}")
        print(f"  Size: {os.path.getsize(unpack_bin)} bytes")
        print(f"  SHA256: {hash_file(unpack_bin)}")

if __name__ == "__main__":
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    pkg_dir = os.path.join(repo_root, "r36s", "build", "portmaster", "nfshp2")
    if not os.path.isdir(pkg_dir):
        print(f"Error: package dir {pkg_dir} does not exist. Run r36s/package.sh first.", file=sys.stderr)
        sys.exit(1)
        
    targets = find_target_port_dir()
    if not targets:
        print("NO_CARD_FOUND")
        sys.exit(2)
        
    print(f"Found target ports directory: {targets[0]}")
    install(targets[0], pkg_dir)
    print("Installation complete!")
