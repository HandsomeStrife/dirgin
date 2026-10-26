#!/bin/bash
# Run by the pcsx2trace systemd user unit (see README). Starts a private Xvfb, then
# PCSX2 on it, loading $STATEFILE. Everything dies with the unit (KillMode=control-group).
# Env: PCSX2_DIR (holds squashfs-root/), ISO, STATEFILE, TRACE_DISPLAY (default :97), LOG.
set -u
D="${TRACE_DISPLAY:-:97}"
n="${D#:}"
rm -f "/tmp/.X${n}-lock" "/tmp/.X11-unix/X${n}"
Xvfb "$D" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
for _ in $(seq 50); do [ -S "/tmp/.X11-unix/X${n}" ] && break; sleep 0.1; done
export DISPLAY="$D" LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
cd "$PCSX2_DIR"
exec ./squashfs-root/AppRun -nogui -statefile "$STATEFILE" "$ISO" >"$LOG" 2>&1
