#!/bin/bash
# Test of install.sh with fake curl, sudo, pkexec and package managers.
# PATH only has the basic tools plus the fakes, so nothing real gets installed.
#   tests/t_install.sh [dist folder]     (default dist/, from packaging/build.sh)
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
DIST=$(cd "${1:-$HERE/dist}" && pwd)
INSTALLER=$HERE/install.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
check() { if [ "$2" = "$3" ] && [ -n "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
has() { if grep -qF -- "$2" "$3"; then echo "ok   $1"; else echo "FAIL $1: no '$2' in $(basename "$3")"; sed 's/^/     /' "$3"; fail=1; fi; }
hasnt() { if grep -qF -- "$2" "$3"; then echo "FAIL $1: '$2' in $(basename "$3")"; fail=1; else echo "ok   $1"; fi; }

for p in 'pkg\.tar\.zst$' '_amd64\.deb$' '\.x86_64\.rpm$'; do
    grep -qE "  maxeco.*$p" "$DIST/SHA256SUMS" || { echo "no $p in $DIST/SHA256SUMS, run packaging/build.sh"; exit 1; }
done

# basic tools, linked from the real system
mkdir -p "$T/base"
for t in bash sh uname id mktemp chmod rm grep sed head awk sha256sum sleep cat basename dirname cp mkdir tr; do
    p=$(command -v "$t") || { echo "missing $t"; exit 1; }
    ln -s "$p" "$T/base/$t"
done

# fake tools, each one writes what it was called with to $LOG
mkfake() { printf '#!/bin/bash\n%s\n' "$2" > "$T/fake/$1"; chmod +x "$T/fake/$1"; }
mkdir -p "$T/fake"
# curl: serves the file with the URL's basename from $SERVE, logs the URL
mkfake curl 'out=; url=; while [ $# -gt 0 ]; do case "$1" in -o) out=$2; shift;; -*) ;; *) url=$1;; esac; shift; done
echo "curl $url" >> "$LOG"; f="$SERVE/${url##*/}"; [ -f "$f" ] || exit 22; cp "$f" "$out"'
mkfake sudo 'if [ "$1" = -n ]; then [ "${SUDO_NOPASS:-1}" = 1 ]; exit; fi; echo "sudo $*" >> "$LOG"; "$@"'
mkfake pkexec 'echo "pkexec $*" >> "$LOG"; "$@"'
for pm in pacman apt-get dpkg dnf zypper yum rpm-ostree; do
    mkfake "$pm" 'echo "'"$pm"' $*" >> "$LOG"; for a; do [ -f "$a" ] && sha256sum "$a" | cut -c1-64 >> "$LOG.sha"; done; exit 0'
done
ln -s "$(command -v cut)" "$T/base/cut"
mkfake pgrep 'echo "pgrep $*" >> "$LOG"; n=$(cat "$T_PGREP" 2>/dev/null || echo 0); [ "$n" -gt 0 ] || exit 1; echo $((n - 1)) > "$T_PGREP"; exit 0'
mkfake pkill 'echo "pkill $*" >> "$LOG"'
mkfake setsid 'echo "setsid $*" >> "$LOG"'

# run NAME TOOLS... -- VAR=VALUE...: install.sh with only base + these fakes
run() {
    local name=$1; shift
    local bin="$T/bin-$name"; mkdir -p "$bin"
    cp -a "$T/base/." "$bin/"
    while [ "$1" != -- ]; do ln -sf "$T/fake/$1" "$bin/$1"; shift; done; shift
    LOG="$T/$name.log"; : > "$LOG"
    env -i PATH="$bin" HOME="$T/home" LOG="$LOG" SERVE="${SERVE:-$DIST}" T_PGREP="$T/$name.pgrep" "$@" \
        bash -c "bash '$INSTALLER' < /dev/null" > "$T/$name.out" 2>&1 < /dev/null
    echo $? > "$T/$name.rc"
}
ALL="curl sudo pkexec pgrep pkill setsid"
SUM() { awk -v p="$1" '$2 ~ p {print $1}' "$DIST/SHA256SUMS"; }

# Arch, latest release from GitHub, tray not running, Wayland session
run arch $ALL pacman dpkg rpm dnf -- WAYLAND_DISPLAY=wayland-0
check "arch: exit 0" "$(cat "$T/arch.rc")" 0
has "arch: SHA256SUMS of the latest GitHub release" "curl https://github.com/gabrielmf1998/MaxEco/releases/latest/download/SHA256SUMS" "$T/arch.log"
has "arch: the pacman package" "releases/latest/download/maxeco-" "$T/arch.log"
has "arch: pacman -U through sudo" "sudo pacman -U --noconfirm /" "$T/arch.log"
check "arch: the installed file is the released one" "$(cat "$T/arch.log.sha")" "$(SUM 'pkg\\.tar\\.zst$')"
has "arch: tray started" "setsid -f /usr/bin/maxeco" "$T/arch.log"
hasnt "arch: nothing killed" "pkill" "$T/arch.log"

# Debian: no pacman, apt-get + dpkg (rpm also present as a tool)
run deb $ALL apt-get dpkg dnf -- WAYLAND_DISPLAY=wayland-0
check "deb: exit 0" "$(cat "$T/deb.rc")" 0
has "deb: apt-get install of the file" "sudo apt-get install -y /" "$T/deb.log"
check "deb: the installed file is the released one" "$(cat "$T/deb.log.sha")" "$(SUM '\\.deb$')"

# Fedora (dnf), openSUSE (zypper), atomic (rpm-ostree)
run fedora $ALL dnf -- DISPLAY=:0
has "fedora: dnf install of the rpm" "sudo dnf install -y /" "$T/fedora.log"
check "fedora: the installed file is the released one" "$(cat "$T/fedora.log.sha")" "$(SUM '\\.rpm$')"
run suse $ALL zypper -- DISPLAY=:0
has "suse: zypper with the unsigned rpm" "sudo zypper --non-interactive install --allow-unsigned-rpm /" "$T/suse.log"
if [ -e /run/ostree-booted ]; then echo "skip rpm-ostree (this machine is atomic)"; fi

# no package manager known
run none $ALL -- DISPLAY=:0
check "none: refuses" "$(cat "$T/none.rc")" 1
has "none: says why" "no pacman, apt, dnf or zypper" "$T/none.out"

# a specific version, GitHub and GitLab
run ver $ALL pacman -- MAXECO_VERSION=v1.2.0 MAXECO_NO_START=1
has "version: GitHub tag URL" "curl https://github.com/gabrielmf1998/MaxEco/releases/download/v1.2.0/SHA256SUMS" "$T/ver.log"
hasnt "version: MAXECO_NO_START" "setsid" "$T/ver.log"
run gl $ALL pacman -- MAXECO_MIRROR=gitlab MAXECO_NO_START=1
has "gitlab: latest permalink" "curl https://gitlab.com/gabriel17166/MaxEco/-/releases/permalink/latest/downloads/SHA256SUMS" "$T/gl.log"
run glv $ALL pacman -- MAXECO_MIRROR=gitlab MAXECO_VERSION=1.2.0 MAXECO_NO_START=1
has "gitlab: tag URL" "curl https://gitlab.com/gabriel17166/MaxEco/-/releases/v1.2.0/downloads/maxeco-" "$T/glv.log"

# no sudo without a password and no terminal: pkexec
run pk $ALL pacman -- SUDO_NOPASS=0 MAXECO_NO_START=1
has "no terminal: pkexec" "pkexec pacman -U --noconfirm /" "$T/pk.log"
hasnt "no terminal: not sudo" "sudo pacman" "$T/pk.log"

# tray already running: SIGTERM (keeps the level) and start the new one
echo 1 > "$T/up.pgrep"
run up $ALL pacman -- WAYLAND_DISPLAY=wayland-0
has "update: old tray ended with SIGTERM" "pkill -TERM -x -u" "$T/up.log"
has "update: new tray started" "setsid -f /usr/bin/maxeco" "$T/up.log"
has "update: says it restarted" "restarting the tray icon" "$T/up.out"

# no graphical session: only a hint
run tty $ALL pacman --
hasnt "no session: tray not started" "setsid" "$T/tty.log"
has "no session: hint" "open MaxEco from the application menu" "$T/tty.out"

# a package that does not match SHA256SUMS is never installed
mkdir -p "$T/bad"; cp "$DIST"/* "$T/bad/"
for f in "$T"/bad/*.pkg.tar.zst; do printf 'x' >> "$f"; done
SERVE="$T/bad" run bad $ALL pacman -- MAXECO_NO_START=1
check "bad sha: refuses" "$(cat "$T/bad.rc")" 1
hasnt "bad sha: pacman never called" "pacman" "$T/bad.log"
has "bad sha: says it" "sha256 does not match" "$T/bad.out"

# a release without this package type
mkdir -p "$T/nodeb"; cp "$DIST"/* "$T/nodeb/"
grep -v '\.deb$' "$DIST/SHA256SUMS" > "$T/nodeb/SHA256SUMS"
SERVE="$T/nodeb" run nodeb $ALL apt-get dpkg -- MAXECO_NO_START=1
check "missing deb: refuses" "$(cat "$T/nodeb.rc")" 1
has "missing deb: says it" "the release has no deb package" "$T/nodeb.out"

# forced format and a local folder
run forced $ALL pacman dnf -- MAXECO_FORMAT=rpm MAXECO_RELEASE_URL=file:///x/dist/ MAXECO_NO_START=1
has "forced rpm: dnf even with pacman" "sudo dnf install -y /" "$T/forced.log"
has "release URL without the trailing slash" "curl file:///x/dist/SHA256SUMS" "$T/forced.log"

[ $fail = 0 ] && echo "ALL OK" || echo "SOME FAILED"
exit $fail
