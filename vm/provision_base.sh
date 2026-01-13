#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BASE_IMAGE=${1:-"${SCRIPT_DIR}/base.qcow2"}

if [[ ! -f "$BASE_IMAGE" ]]; then
  echo "Base image not found: $BASE_IMAGE" >&2
  exit 1
fi

if ! command -v virt-customize >/dev/null 2>&1; then
  echo "Missing dependency: virt-customize (install: sudo apt install -y libguestfs-tools)" >&2
  exit 1
fi

# Speed up libguestfs on some hosts
: "${LIBGUESTFS_BACKEND:=direct}"
export LIBGUESTFS_BACKEND

echo "Provisioning base image: $BASE_IMAGE"
virt-customize -a "$BASE_IMAGE" \
  --run-command 'apt-get update' \
  --run-command 'DEBIAN_FRONTEND=noninteractive apt-get install -y openssh-server qemu-guest-agent curl ffmpeg netcat-openbsd v4l-utils' \
  --run-command 'ssh-keygen -A' \
  --run-command 'id -u cloud >/dev/null 2>&1 || useradd -m -s /bin/bash cloud' \
  --run-command 'echo "cloud:cloud" | chpasswd' \
  --run-command 'sed -i "s/^#PasswordAuthentication yes/PasswordAuthentication yes/" /etc/ssh/sshd_config' \
  --run-command 'printf "\nUseDNS no\nPrintMotd no\n" >> /etc/ssh/sshd_config' \
  --run-command 'systemctl enable ssh || true' \
  --run-command 'systemctl enable qemu-guest-agent || true' \
  --run-command 'mkdir -p /opt/cloudphone && echo provisioned > /opt/cloudphone/provisioned'
echo "Provisioning completed."
