# tendedero (Wayland)
Screenshots, hung out to dry — a Wayland port of the clothesline-screenshot inspired by https://github.com/alejandrobujan/tendedero.
C + GTK3 + `gtk-layer-shell`, ~800 lines, one file.

## Build (Ubuntu 24.04+)

    sudo apt install build-essential pkg-config libgtk-3-dev libgtk-layer-shell-dev
    make && sudo make install          # or just ./tendedero

Optional: `wl-clipboard` (more reliable copy on layer surfaces), `swappy` or `satty` (markup).

## Use

    tendedero &                   # start the daemon (autostart this)
    tendedero --capture           # interactive screenshot (xdg-desktop-portal) -> hangs on the line
    tendedero --add shot.png      # hang any image
    tendedero --settings          # settings window
    tendedero --sample            # hang a generated test card
    tendedero --quit

Anything saved into the watch folder (default `~/Pictures/Screenshots`) also gets hung,
so your compositor's own screenshot key, Spectacle, Flameshot, etc. just work.

Key bindings:

    # sway / river-style
    bindsym Print exec tendedero --capture
    # or, wlroots without a screenshot portal:
    #   capture_cmd=sh -c 'f=$(mktemp --suffix=.png); grim -g "$(slurp)" "$f" && tendedero --add "$f"'
    # Hyprland
    bind = , Print, exec, tendedero --capture

## Interaction

| Action            | Result                                              |
|-------------------|-----------------------------------------------------|
| hover / brush past| card grows slightly and swings with your pointer    |
| click             | copy image to clipboard ("✓ Copied" badge)          |
| drag              | drag the file/image into any app (chat, editor...)  |
| hold (0.55 s)     | mark up: `swappy`, `satty`, or `xdg-open`           |
| right-click       | let it go: the peg drops and the card falls away    |

## Settings (`~/.config/tendedero/config.ini`, also live in the UI)

* `offset`    — distance from the top of the screen to the wire. **Default 140.**
* `reserve`   — `false`: OVERLAY layer, always on top, reserves nothing.
                `true`: TOP layer with an exclusive zone; tiled windows start below the line.
* `stiffness` (160), `damping` ratio (0.35), `kick` (1.0) — spring physics.
* `watch`, `watch_dir`, `capture_cmd`, `markup_cmd`.

## Compositor support

Needs `wlr-layer-shell`: **Sway, Hyprland, KDE Plasma (Wayland), river, labwc, COSMIC, niri**.
**GNOME/Mutter does not implement layer-shell** (Ubuntu's default desktop). There the app falls
back to a plain full-width dock-hinted window: it draws, but cannot reserve space or guarantee
being on top. Use a layer-shell compositor, or the GNOME shell-extension route, for the real thing.

## How the spring works

Every card is a damped torsional spring pendulum hanging from a peg (`θ'' = -k(θ-θeq) - cθ'`).
When neighbours slide aside on their own position springs, the peg's acceleration shifts `θeq`,
so cards lag and overshoot like real laundry. Entry, hover-scale and drop use the same spring
integrator with 240 Hz sub-stepping.
