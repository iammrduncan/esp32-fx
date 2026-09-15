#!/bin/sh
# Prepare a NEW demo jail. Run as root (rootless container root is fine on a host).
set -eu
project=${1:?usage: prepare-jail.sh PROJECT ABSOLUTE_NEW_JAIL [--bind]}
jail=${2:?usage: prepare-jail.sh PROJECT ABSOLUTE_NEW_JAIL [--bind]}
mode=${3:---copy}
test "$(id -u)" = 0 || { echo 'requires root inside the device/container' >&2; exit 1; }
case "$jail" in /*) ;; *) echo 'jail must be absolute' >&2; exit 1;; esac
test ! -e "$jail" || { echo 'jail already exists; use a fresh directory' >&2; exit 1; }
case "$mode" in --copy|--bind) ;; *) echo 'expected --copy or --bind' >&2; exit 1;; esac
for file in SPEC.md battery.sh test.sh; do test -f "$project/$file"; done
shell=/usr/bin/dash
test -x "$shell" || shell=/bin/dash
test -x "$shell"
umask 022
mkdir -p "$jail/bin" "$jail/project" "$jail/lib" "$jail/usr/lib"
chmod 0755 "$jail" "$jail/bin" "$jail/project" "$jail/lib" "$jail/usr" "$jail/usr/lib"
for file in SPEC.md battery.sh test.sh; do
    cp "$project/$file" "$jail/project/$file"
    chmod 0644 "$jail/project/$file"
done
if test "$mode" = --bind; then
    # Preserve firmware-backed executable/library mappings on the NOMMU board.
    # This jail contains only dash, its libraries, and the small project.
    : > "$jail/bin/sh"
    mount -o bind "$shell" "$jail/bin/sh"
    mount -o remount,bind,ro "$jail/bin/sh"
    mount -o bind /lib "$jail/lib"
    mount -o remount,bind,ro "$jail/lib"
    # The target dash also depends on the firmware's libfork.so.0 in /usr/lib.
    mount -o bind /usr/lib "$jail/usr/lib"
    mount -o remount,bind,ro "$jail/usr/lib"
else
    cp "$shell" "$jail/bin/sh"
    # Workstation-only copy mode, for the trusted system dash (never model code).
    ldd "$shell" | awk '/=> \// { print $3 } /^[[:space:]]*\// { print $1 }' |
    while IFS= read -r library; do
        mkdir -p "$jail$(dirname "$library")"
        cp -L "$library" "$jail$library"
    done
fi
printf 'Prepared isolated example at %s\n' "$jail"
