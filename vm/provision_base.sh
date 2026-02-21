#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BASE_IMAGE=${1:-"${SCRIPT_DIR}/base.qcow2"}

if [[ ! -f "$BASE_IMAGE" ]]; then
  echo "Base image not found: $BASE_IMAGE" >&2
  exit 1
fi

if ! command -v virt-customize >/dev/null 2>&1; then
  echo "Missing dependency: virt-customize (install: sudo dnf install -y libguestfs-tools || sudo apt install -y libguestfs-tools)" >&2
  exit 1
fi

# Speed up libguestfs on some hosts
: "${LIBGUESTFS_BACKEND:=direct}"
export LIBGUESTFS_BACKEND

echo "Provisioning base image: $BASE_IMAGE"
virt-customize -a "$BASE_IMAGE" \
  --run-command 'set -e; if command -v apt-get >/dev/null 2>&1; then apt-get update; DEBIAN_FRONTEND=noninteractive apt-get install -y openssh-server qemu-guest-agent curl ffmpeg netcat-openbsd v4l-utils; elif command -v dnf >/dev/null 2>&1; then dnf -y install epel-release || true; dnf -y install openssh-server qemu-guest-agent curl nmap-ncat v4l-utils NetworkManager dhcp-client || true; dnf -y install ffmpeg || true; elif command -v yum >/dev/null 2>&1; then yum -y install epel-release || true; yum -y install openssh-server qemu-guest-agent curl nmap-ncat v4l-utils NetworkManager dhcp-client || true; yum -y install ffmpeg || true; fi' \
  --run-command 'ssh-keygen -A' \
  --run-command 'id -u cloud >/dev/null 2>&1 || useradd -m -s /bin/bash cloud' \
  --run-command 'echo "cloud:cloud" | chpasswd' \
  --run-command 'sed -i "s/^#PasswordAuthentication yes/PasswordAuthentication yes/" /etc/ssh/sshd_config' \
  --run-command 'printf "\nUseDNS no\nPrintMotd no\nListenAddress 0.0.0.0\nListenAddress ::\n" >> /etc/ssh/sshd_config' \
  --run-command 'systemctl enable ssh || true' \
  --run-command 'systemctl enable sshd || true' \
  --run-command 'systemctl enable NetworkManager || true' \
  --run-command 'touch /etc/cloud/cloud-init.disabled' \
  --run-command 'systemctl disable cloud-init cloud-init-local cloud-config cloud-final cloud-init.target || true' \
  --run-command 'systemctl mask cloud-init cloud-init-local cloud-config cloud-final || true' \
  --run-command 'rm -f /etc/sysconfig/network-scripts/ifcfg-* || true' \
  --run-command 'systemctl disable cloudphone-dhcp.service || true' \
  --run-command 'rm -f /etc/systemd/system/cloudphone-dhcp.service || true' \
  --run-command 'rm -f /etc/systemd/system/multi-user.target.wants/cloudphone-dhcp.service || true' \
  --run-command 'systemctl disable firewalld || true' \
  --write '/etc/NetworkManager/system-connections/cloudphone-eth0.nmconnection:[connection]
id=cloudphone-eth0
type=ethernet
interface-name=eth0
autoconnect=true

[ipv4]
method=auto

[ipv6]
method=auto
addr-gen-mode=stable-privacy

[ethernet]

[proxy]
' \
  --chmod '0600:/etc/NetworkManager/system-connections/cloudphone-eth0.nmconnection' \
  --run-command 'systemctl enable qemu-guest-agent || true' \
  --run-command 'mkdir -p /opt/cloudphone && echo provisioned > /opt/cloudphone/provisioned'
echo "Provisioning completed."
