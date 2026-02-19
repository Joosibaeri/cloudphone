This project contains the server component of CloudPhone. The server is written in C and runs on Linux, handling all processing and user account management while communicating with a small ARM client via SSH.

User accounts are stored under `/userdata/accounts`, and the server automatically creates required directories on first launch. The main source file is `main.c`. Virtual machine-related files are stored in the `vm/` folder and include base root filesystems useful for testing.

Build & run
--------------

The preferred way to build the server is via the provided Makefile which produces an executable named `main`:

```sh
make         # builds `main` from main.c
./main       # run the server
```

If you want the compatibility name used previously, `make cloudphone_server` will produce an executable named `cloudphone_server`.

Rocky Linux
------------
- The default base image is now Rocky Linux 9 GenericCloud. You can change `base_image_url` in [config.cfg](config.cfg) to any other qcow2 cloud image.
- Provisioning supports both `apt-get` and `dnf/yum` inside the guest image.

CloudPhone is designed for controlled environments, keeping the client minimal while the server handles most of the logic and data management.

IPv6
-----
- In `ipv6` mode, the VM still forwards SSH on IPv4 (e.g., `ssh -p <port> cloud@127.0.0.1`) and a socat helper bridges IPv6 connections to the same port. Use `ssh -6 -p <port> cloud@::1` for local IPv6.
- Dynamic port allocation still uses IPv6-only bindings for the helper forwarder; ensure IPv6 clients can reach the server address printed by `serverip`.

Network mode
------------
- `network_mode=bridge` (default) starts QEMU with a Linux bridge (`bridge_name`, default `br0`). This gives each VM its own network identity/IP (typically via DHCP) and you connect directly with `ssh cloud@<vm-ip>` on port 22.
- `network_mode=user` keeps QEMU user-mode networking with host port forwarding.