#!/usr/bin/env bash
# MaxEco installer, https://github.com/gabrielmf1998/MaxEco
#
#   curl -fsSL https://raw.githubusercontent.com/gabrielmf1998/MaxEco/main/install.sh | bash
#
# Options go on the bash side of the pipe, for example
#   curl -fsSL .../install.sh | MAXECO_MIRROR=gitlab bash
#
#   MAXECO_VERSION=1.2.0    that release instead of the latest one
#   MAXECO_MIRROR=gitlab    download from the GitLab mirror instead of GitHub
#   MAXECO_FORMAT=deb       force the package type (arch, deb or rpm)
#   MAXECO_NO_START=1       do not start the tray icon at the end
#   MAXECO_RELEASE_URL=...  any other folder with the release files (also file://)
#
# It picks the package for this distro from the release (.pkg.tar.zst for
# pacman, .deb for apt, .rpm for dnf, zypper and rpm-ostree), installs it only
# if its sha256 matches the release's SHA256SUMS, and uses the distro's own
# package manager, so removing it later is the usual pacman -R, apt remove or
# dnf remove. Root is asked for once, with sudo in a terminal and pkexec
# otherwise. Running it again updates MaxEco and restarts the tray icon, which
# keeps the level that was on.
set -euo pipefail

GITHUB="https://github.com/gabrielmf1998/MaxEco"
GITLAB="https://gitlab.com/gabriel17166/MaxEco"

msg()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

release_url() {
    local version="${MAXECO_VERSION:-latest}" mirror="${MAXECO_MIRROR:-github}"
    version="${version#v}"
    if [ -n "${MAXECO_RELEASE_URL:-}" ]; then
        printf '%s\n' "${MAXECO_RELEASE_URL%/}"
        return
    fi
    case "$mirror:$version" in
        gitlab:latest) echo "$GITLAB/-/releases/permalink/latest/downloads" ;;
        gitlab:*)      echo "$GITLAB/-/releases/v$version/downloads" ;;
        github:latest) echo "$GITHUB/releases/latest/download" ;;
        github:*)      echo "$GITHUB/releases/download/v$version" ;;
        *)             die "MAXECO_MIRROR must be github or gitlab" ;;
    esac
}

# which package type: the package manager decides, not the distro name, and
# pacman comes first because Arch can have dpkg and rpm installed as tools
detect_format() {
    if [ -n "${MAXECO_FORMAT:-}" ]; then
        case "$MAXECO_FORMAT" in
            arch|pacman) echo arch ;;
            deb|rpm)     echo "$MAXECO_FORMAT" ;;
            *)           die "MAXECO_FORMAT must be arch, deb or rpm" ;;
        esac
        return
    fi
    if command -v pacman >/dev/null; then echo arch
    elif command -v apt-get >/dev/null && command -v dpkg >/dev/null; then echo deb
    elif command -v rpm-ostree >/dev/null && [ -e /run/ostree-booted ]; then echo rpm
    elif command -v dnf >/dev/null || command -v zypper >/dev/null || command -v yum >/dev/null; then echo rpm
    else die "no pacman, apt, dnf or zypper here. Build from source instead: $GITHUB#install"
    fi
}

package_pattern() {
    case "$1" in
        arch) echo 'maxeco-[0-9][0-9.]*-[0-9]+-x86_64\.pkg\.tar\.zst' ;;
        deb)  echo 'maxeco_[0-9][0-9.]*-[0-9]+_amd64\.deb' ;;
        rpm)  echo 'maxeco-[0-9][0-9.]*-[0-9]+\.x86_64\.rpm' ;;
    esac
}

# root for one command: already root, sudo when there is a terminal to type
# the password in (or sudo needs none), pkexec otherwise (a desktop dialog)
as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif [ -n "${MAXECO_SUDO:-}" ]; then
        $MAXECO_SUDO "$@"
    elif command -v sudo >/dev/null && { sudo -n true 2>/dev/null || ( : </dev/tty ) 2>/dev/null; }; then
        sudo "$@"
    elif command -v pkexec >/dev/null; then
        pkexec "$@"
    else
        die "this needs root: run it as root, or install sudo or pkexec"
    fi
}

install_package() {
    local format="$1" file="$2"
    case "$format" in
        arch)
            as_root pacman -U --noconfirm "$file" ;;
        deb)
            # apt resolves the dependencies; the path needs a slash to be read as a file
            as_root apt-get install -y "$file" ;;
        rpm)
            if command -v rpm-ostree >/dev/null && [ -e /run/ostree-booted ]; then
                as_root rpm-ostree install -y "$file"
                RPM_OSTREE=1
            elif command -v dnf >/dev/null; then
                as_root dnf install -y "$file"
            elif command -v zypper >/dev/null; then
                as_root zypper --non-interactive install --allow-unsigned-rpm "$file"
            else
                as_root yum install -y "$file"
            fi ;;
    esac
}

# the tray ends on SIGTERM keeping the level (it does not go back to Normal),
# so a new version can take over while Eco or MaxEco stays on
start_tray() {
    if [ "${MAXECO_NO_START:-0}" = 1 ] || [ "${RPM_OSTREE:-0}" = 1 ]; then return; fi
    if [ "$(id -u)" -eq 0 ] || [ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
        msg "open MaxEco from the application menu to get the tray icon"
        return
    fi
    local uid; uid=$(id -u)
    if pgrep -x -u "$uid" maxeco >/dev/null 2>&1; then
        pkill -TERM -x -u "$uid" maxeco 2>/dev/null || true
        local i
        for i in 1 2 3 4 5 6 7 8 9 10; do
            pgrep -x -u "$uid" maxeco >/dev/null 2>&1 || break
            sleep 0.3
        done
        msg "restarting the tray icon with the new version"
    else
        msg "starting the tray icon"
    fi
    setsid -f /usr/bin/maxeco </dev/null >/dev/null 2>&1 || warn "could not start maxeco, open it from the application menu"
}

main() {
    [ "$(uname -s)" = Linux ] || die "MaxEco is for Linux"
    [ "$(uname -m)" = x86_64 ] || die "the packages are for x86_64. On $(uname -m) build from source with make LIBC=1: $GITHUB#install"
    [ -d /run/systemd/system ] || warn "systemd is not running here, MaxEco needs it to change levels"
    command -v curl >/dev/null || die "curl not found"
    command -v sha256sum >/dev/null || die "sha256sum not found (coreutils)"

    local format url pattern name
    format=$(detect_format)
    url=$(release_url)
    pattern=$(package_pattern "$format")
    # global, the EXIT trap runs after main has returned
    tmp=$(mktemp -d)
    trap 'rm -rf "${tmp:-}"' EXIT
    # apt reads local packages as its _apt user, so the folder must be readable
    chmod 755 "$tmp"

    msg "release files from $url"
    curl -fsSL "$url/SHA256SUMS" -o "$tmp/SHA256SUMS" || die "could not download SHA256SUMS from $url"
    name=$(grep -oE "  $pattern\$" "$tmp/SHA256SUMS" | sed 's/^  //' | head -1 || true)
    [ -n "$name" ] || die "the release has no $format package"
    msg "downloading $name"
    curl -fL --progress-bar "$url/$name" -o "$tmp/$name" || die "could not download $name"
    chmod 644 "$tmp/$name"
    ( cd "$tmp" && awk -v n="$name" '$2 == n' SHA256SUMS | sha256sum --quiet -c - ) \
        || die "sha256 does not match SHA256SUMS, nothing was installed"
    msg "sha256 checked"

    msg "installing with the package manager (asks for root once)"
    install_package "$format" "$tmp/$name"

    if [ "${RPM_OSTREE:-0}" = 1 ]; then
        msg "done. Reboot to finish (rpm-ostree), then open MaxEco from the application menu"
        return
    fi
    start_tray
    case "$format" in
        arch) msg "MaxEco installed. To remove it later: sudo pacman -R maxeco" ;;
        deb)  msg "MaxEco installed. To remove it later: sudo apt remove maxeco" ;;
        rpm)  msg "MaxEco installed. To remove it later: sudo dnf remove maxeco (or zypper remove maxeco)" ;;
    esac
}

main "$@"
