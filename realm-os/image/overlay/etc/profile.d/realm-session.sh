# Boot straight into the realm on the first virtual terminal.
if [ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
    # Software rendering keeps virtual machines and unsupported GPUs working.
    if [ ! -e /dev/dri/renderD128 ]; then
        export WLR_RENDERER=pixman
        export WLR_NO_HARDWARE_CURSORS=1
    fi
    exec /usr/local/bin/realmd >"$HOME/.realmd.log" 2>&1
fi
