# RealmOS

An operating system where the desktop session is an MMO. Each installation is a
character. The PC boots into a shared world, and your character has a home with
a desk in it. Sit at the desk and the world dims behind a normal desktop running
real applications. Stand up and you're back outside.

This is the first playable version. It boots, and the game is a real Wayland
compositor running real Linux applications. The world is currently a top-down
2D renderer. The 3D version plugs into the same plumbing (see below).

## What's real

| In the game | On the machine |
|---|---|
| Your character | This installation: id derived from `/etc/machine-id`, saved in `~/.local/share/realm/character` |
| Class and attributes | Hardware: CPU cores → STR, memory → INT, display width → PER; laptops are Wanderers |
| Sitting at your desk | The desktop: `realmd` shows and focuses Wayland app windows |
| Creatures orbiting your home | Your processes, read from `/proc`, sized by memory |
| A red creature chasing you | A runaway process (sustained high CPU, or >25% of RAM) |
| Swatting it | `SIGTERM` to that pid (session-critical processes are protected) |
| Dropping an item | Put a file (≤32 KB) in `~/Outbox`; it leaves the disk and lies outside your door |
| Picking an item up | The file is sent over the network and written to `~/Inventory` |
| Other players | Other RealmOS machines on your LAN (UDP broadcast), or anywhere via a relay |

A file lying in the world stays stored on the machine that dropped it until
someone picks it up.

## Layout

- `realmd/` — the session: a wlroots 0.17 compositor written in C. `main.c` handles
  windows and input, `game.c` the world, `procs.c` the process creatures,
  `net.c` the realm protocol, `character.c` identity and the save file.
- `server/realm-relay.py` — optional relay so installations can meet across the internet.
- `image/` — builds a bootable live ISO on Ubuntu 24.04: autologin on tty1 → `realmd`.

## Build and run

```sh
sudo apt install libwlroots-dev libcairo2-dev libxkbcommon-dev wayland-protocols meson foot
cd realmd && meson setup build && ninja -C build
./build/realmd            # from a TTY (takes over the display), or nested inside another Wayland session
```

Bootable image (on an Ubuntu 24.04 host, as root):

```sh
sudo apt install debootstrap squashfs-tools grub-pc-bin grub-efi-amd64-bin mtools xorriso
sudo image/build-iso.sh   # → image/out/realmos.iso, BIOS and UEFI
sudo dd if=image/out/realmos.iso of=/dev/sdX bs=4M status=progress
```

The live image logs in as `player` (password `realm`). A live USB forgets
everything on reboot, so each boot is a new character. A disk installer is on
the roadmap.

## Controls

Outside: WASD or arrows (or click) to move, E / Space to interact, T or Enter to chat.
At the desk: Super+Esc to stand up, Super+Enter terminal, Super+E files, Super+B browser,
Super+Tab cycle windows, Super+Q close, Super+F maximize, Super+drag to move a window.
Anywhere: Ctrl+Alt+Backspace to restart the session, Ctrl+Alt+F1–F12 to switch VT.

## Toward the 3D version

`realmd` is the layer that stays: it owns the display, the input devices and the
app windows, and runs the game rules. The 3D world replaces `game_draw_world`
and adds a proper renderer. App windows become textures on the monitor mesh in
your 3D home, so the "sit down" transition can be a camera move into the screen.
