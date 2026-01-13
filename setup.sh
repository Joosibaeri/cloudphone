#!/bin/bash
set -e

BASE_DIR="/userdata/base"
IMG_FILE="$BASE_DIR/base.qcow2"
DEFAULT_URL="https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-genericcloud-amd64.qcow2"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--url URL] [--force]

Download or update the base QCOW2 image used by the server.

Options:
  -u, --url URL   Download image from URL (default: $DEFAULT_URL)
  -f, --force     Overwrite existing $IMG_FILE
  -h, --help      Show this help
EOF
}

if [ "$#" -gt 0 ]; then
    URL=""
    FORCE=0
    while [ $# -gt 0 ]; do
        case "$1" in
            -u|--url) URL="$2"; shift 2;;
            -f|--force) FORCE=1; shift;;
            -h|--help) usage; exit 0;;
            *) echo "Unknown option: $1"; usage; exit 1;;
        esac
    done
else
    URL=""
    FORCE=0
fi

if [ -z "$URL" ]; then
    URL="$DEFAULT_URL"
fi

mkdir -p "$BASE_DIR"

if [ -f "$IMG_FILE" ] && [ "$FORCE" -ne 1 ]; then
    echo "Base image already exists at $IMG_FILE — use --force to replace."
    exit 0
fi

echo "Downloading base image from: $URL"
# create a temporary filename in BASE_DIR
tmp=$(mktemp "${BASE_DIR}/baseimg.XXXXXX") || { echo "Failed to create temporary file"; exit 1; }
# try curl then wget
if command -v curl >/dev/null 2>&1; then
    curl -L --fail -o "$tmp" "$URL" || { echo "Download failed"; rm -f "$tmp"; exit 1; }
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$tmp" -o /dev/null "$URL" || { echo "Download failed"; rm -f "$tmp"; exit 1; }
else
    echo "Error: neither curl nor wget is available to download images." >&2
    exit 1
fi

if [ ! -s "$tmp" ]; then echo "Downloaded file empty"; rm -f "$tmp"; exit 1; fi

if [ -f "$IMG_FILE" ]; then
    echo "Backing up old base to ${IMG_FILE}.bak"
    mv "$IMG_FILE" "${IMG_FILE}.bak" || { echo "Failed to backup old file"; rm -f "$tmp"; exit 1; }
fi

mv "$tmp" "$IMG_FILE" || { echo "Failed to move downloaded file to $IMG_FILE"; rm -f "$tmp"; exit 1; }
chmod 0644 "$IMG_FILE" || true
echo "Base image is ready at: $IMG_FILE"
