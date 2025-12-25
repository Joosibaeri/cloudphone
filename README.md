# CloudPhone (GUI)

A small GTK-based client that connects to the CloudPhone server without any terminal interaction. It calculates the user-specific SSH port, can start an optional camera stream via ffmpeg, and shows SSH logs inside the window.

## Build

Requires GTK 3, ssh, and ffmpeg development/runtime packages.

```sh
gcc main.c -o cloudphone $(pkg-config --cflags --libs gtk+-3.0)
```

## Usage

1. Start the app: `./cloudphone` (run in any terminal or desktop launcher; the UI handles all interaction).
2. Enter the server IP. User defaults to `cloud`; the port field is pre-filled from the user-based calculation but can be overridden.
3. (Optional) Enable the camera stream, set camera port and device (default `/dev/video0`).
4. Click **Verbinden**. Connection reachability is checked, then camera (if enabled) and SSH start. Output appears in the log area.
5. Click **Stop** to terminate SSH and the camera stream.

Notes:
- SSH is started with `BatchMode=yes` and `StrictHostKeyChecking=accept-new`; use key-based auth or ensure host keys are set up. No interactive password prompts are shown.
- The app does not rely on prebuilt `_x86` binaries; compile locally for your platform.
