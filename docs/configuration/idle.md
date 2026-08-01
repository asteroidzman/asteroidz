---
title: Idle
description: Screen blanking, locking and suspend, without a separate idle daemon.
---

# Idle

asteroidz implements `ext-idle-notify-v1` and owns DPMS, and the bar is a
Wayland client that can hold an idle timer and dispatch to the compositor. An
external daemon between the two — swayidle and friends — was only ever
translating one into the other, with its own config file and a set of timeouts
nothing else in the desktop could read.

So the timeouts live here, in the compositor's config, and the bar carries them
out. Change one and reload; nothing needs restarting.

```kdl
bar {
    idle {
        enable true
        dpms-timeout 600            // screen off after 10 minutes
        lock-timeout 0              // never lock on idle
        suspend-timeout 0           // never suspend on idle
        lock-before-suspend false
        respect-inhibitors true
        lock-command "swaylock -f"
        on-idle ""
        on-resume "~/.config/scripts/audio-resync.sh"
    }
}
```

| Setting | Default | Description |
| :--- | :--- | :--- |
| `enable` | `false` | Act on idle at all. Off leaves every timeout below inert. |
| `dpms-timeout` | `0` | Seconds of inactivity before the outputs power down. `0` never does. Any input wakes them. |
| `lock-timeout` | `0` | Seconds before `lock-command` runs. `0` never locks. |
| `suspend-timeout` | `0` | Seconds before the machine suspends. `0` never suspends. |
| `lock-before-suspend` | `0` | Run the lock command on the way down, so the machine comes back locked even when the lock timeout is longer than the suspend one. |
| `respect-inhibitors` | `1` | Honour idle-inhibitors, which is how a video player holds sleep off. Off makes the timeouts absolute. |
| `lock-command` | `` | What locking means on this machine. Empty makes `lock-timeout` and `lock-before-suspend` do nothing. |
| `on-idle` | `` | Runs alongside the built-in idle actions, not instead of them. |
| `on-resume` | `` | Runs when activity returns, after the outputs are back. |

Every timeout is in **seconds**, and `0` means never. Twelve hours is the cap:
past that it is not a timeout, it is "off", which `0` already says.

## The timeouts are independent

Each one is its own idle timer, not a chain. With `dpms-timeout 600` and
`suspend-timeout 1800` the screen goes off at ten minutes and the machine
suspends at thirty — the suspend timer is not counting from the blank. That
also means a shorter lock timeout than DPMS timeout locks first and blanks
after, which is usually what you want.

Activity cancels all of them at once and, if the outputs were off, brings them
back before `on-resume` runs.

## `on-idle` and `on-resume`

These exist because some machines need something the compositor has no business
knowing. This desktop, for instance, re-locks an S/PDIF DAC after the outputs
re-clock on wake — a hardware quirk, not a window-manager feature. They run in
addition to the built-in actions rather than replacing them, so a resume hook
does not have to re-implement turning the screens back on.

## Inhibitors

`respect-inhibitors` covers the Wayland protocol's own inhibitors, which is what
a video player uses. See also
[`idleinhibit_ignore_visible`](/docs/configuration/miscellaneous), which decides
whether a client that is not visible may inhibit at all.

The bar's idle pill — the cup — is **not** one of those, and turning
`respect-inhibitors` off does not turn it off. It flips the compositor's own
manual inhibit (`toggle_idle_inhibit`), which is a deliberate instruction from
the person at the keyboard rather than a request from a program; the option is
about programs. So "stop the browser holding my screen awake" and "keep my
screen awake right now" remain separate answers to separate questions.

The pill shows the compositor's state rather than remembering its own, so a
keybind bound to `toggle_idle_inhibit` and the cup always agree, and restarting
the bar does not lose an inhibit that is still in force. `amsg get idle` returns
the same two fields it reads (`inhibited`, `manual`), and `amsg watch idle`
streams them.

It is hidden when nothing would idle anyway — `enable false`, or every timeout
at `0`. A "keep awake" button in a session where the screen never sleeps is
describing something that is not happening.

## Replacing swayidle

Delete the daemon and its config; there is nothing to keep. A `timeout 600
'dpms off' resume 'dpms on'` line becomes `dpms-timeout 600`, and anything the
resume command did beyond restoring the outputs becomes `on-resume`.
