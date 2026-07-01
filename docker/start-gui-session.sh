#!/bin/bash
#
# start-gui-session.sh — entrypoint for Dockerfile.linux-gui.
#
# Boots a virtual desktop (Xvfb + openbox + xterm), starts a VNC server so a
# human can connect and drive it, seeds a small config + history so the popup
# isn't empty on first open, then runs the daemon in the foreground (so its
# logs show up in `docker logs`).

set -e

echo "Starting virtual X server..."
Xvfb :99 -screen 0 1280x800x24 &
sleep 1
export DISPLAY=:99

echo "Starting window manager (openbox)..."
openbox &
sleep 1

echo "Starting a terminal (xterm) so you have somewhere to copy text from..."
xterm -geometry 100x30+40+40 &

# Use a friendlier hotkey for manual testing over VNC: Ctrl+Alt+V. The
# project's default is Cmd+Shift+V (Super+Shift+V on Linux), but a genuine
# Super keypress isn't reliably deliverable through every VNC client — macOS
# Screen Sharing in particular doesn't consistently forward Command as Super
# to a Linux VNC server. Ctrl+Alt+V is something any keyboard/VNC client
# combination can send unambiguously. Edit this file and restart the
# container to try a different combination.
mkdir -p "$HOME/.local/share/clipboard-manager"
cat > "$HOME/.local/share/clipboard-manager/config.txt" <<'EOF'
hotkey=ctrl+alt+v
EOF

# Seed a little history so the popup isn't empty the first time you open it.
cat > "$HOME/.local/share/clipboard-manager/history.txt" <<'EOF'
1700000003|this is a seeded test entry
1700000002|copy anything in the xterm window to add more entries
1700000001|press Ctrl+Alt+V to open the popup
EOF

echo "Starting VNC server on port 5900 (no password)..."
x11vnc -display :99 -forever -shared -nopw -rfbport 5900 &

echo ""
echo "======================================================================"
echo " Ready. Connect with any VNC client to port 5900, e.g. on macOS:"
echo "   open vnc://localhost:5900"
echo ""
echo " Hotkey is Ctrl+Alt+V (not the project default Cmd+Shift+V — see"
echo " this script's comment for why)."
echo "======================================================================"
echo ""

cd /app
exec ./build/clipboard-manager -d --foreground
