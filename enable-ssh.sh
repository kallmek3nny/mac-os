#!/bin/sh
#
# enable-ssh.sh -- give the Windows machine SSH access to this laptop, so the
# capture work can be driven directly instead of copy-pasting output back and
# forth.
#
# Run ONCE on the LAPTOP, from the desktop:
#
#     sudo sh enable-ssh.sh
#
# It installs and starts sshd, authorises the Windows key that is baked in
# below, and prints the exact command to run on the Windows side.
#
# SAFE TO RE-RUN. It is idempotent -- the key is only appended if absent.
#
# WHAT THIS OPENS: password login stays DISABLED, so only a holder of the
# matching private key can connect. Reachable from your LAN only; nothing is
# exposed to the internet unless you forward a port on your router, which this
# script does not do.
#
# To revoke access completely when the project is finished:
#
#     sudo sh enable-ssh.sh --undo
#

set -u

trap '' HUP

# The public key from the Windows machine. The matching PRIVATE key never
# leaves that machine -- this half is not a secret.
PUBKEY="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPFGC+QqUOIS/ZqcceE8rN1uQzFwNKE08XGOtCgNTb3T kentl-windows"

say() { echo "$@"; }

# ------------------------------------------------------------- must be root --
if [ "$(id -u)" -ne 0 ]; then
  say "FAIL   : run this with sudo:  sudo sh $0"
  exit 1
fi

#
# Work out who the real desktop user is. Under sudo, $HOME is /root, which is
# NOT where the key should land -- the Windows side will log in as this user.
#
WHO="${SUDO_USER:-}"
if [ -z "$WHO" ] || [ "$WHO" = root ]; then
  WHO=$(logname 2>/dev/null || echo "")
fi
if [ -z "$WHO" ] || [ "$WHO" = root ]; then
  say "FAIL   : could not determine your normal (non-root) username."
  say "         Run this with sudo from your own login, not as root."
  exit 1
fi

HOMEDIR=$(getent passwd "$WHO" | cut -d: -f6)
if [ -z "$HOMEDIR" ] || [ ! -d "$HOMEDIR" ]; then
  say "FAIL   : no home directory found for user '$WHO'"
  exit 1
fi

AUTH="$HOMEDIR/.ssh/authorized_keys"
DROPIN=/etc/ssh/sshd_config.d/zz-mmiotrace.conf

# -------------------------------------------------------------------- undo --
if [ "${1:-}" = "--undo" ]; then
  say "=== enable-ssh.sh --undo ==="

  if [ -f "$AUTH" ]; then
    grep -vF "$PUBKEY" "$AUTH" > "$AUTH.tmp" 2>/dev/null || : > "$AUTH.tmp"
    mv "$AUTH.tmp" "$AUTH"
    chown "$WHO" "$AUTH" 2>/dev/null
    chmod 600 "$AUTH"
    say "PASS   : removed the Windows key from $AUTH"
  else
    say "note   : $AUTH does not exist"
  fi

  [ -f "$DROPIN" ] && rm -f "$DROPIN" && say "PASS   : removed $DROPIN"

  systemctl restart ssh 2>/dev/null || systemctl restart sshd 2>/dev/null
  say "PASS   : sshd restarted"
  say ""
  say "The Windows machine can no longer log in. sshd itself is left running;"
  say "to stop it entirely:  sudo systemctl disable --now ssh"
  exit 0
fi

# ----------------------------------------------------------------- install --
say "=== enable-ssh.sh $(date -u '+%Y-%m-%d %H:%M UTC') ==="
say "user   : $WHO"
say "home   : $HOMEDIR"
say ""

# -- 1. sshd present ----------------------------------------------------------
if [ -x /usr/sbin/sshd ]; then
  say "PASS   : sshd already installed"
else
  say "note   : installing openssh-server..."
  if DEBIAN_FRONTEND=noninteractive apt-get install -y openssh-server >/dev/null 2>&1; then
    say "PASS   : openssh-server installed"
  else
    say "note   : first attempt failed, refreshing package lists..."
    apt-get update >/dev/null 2>&1
    if DEBIAN_FRONTEND=noninteractive apt-get install -y openssh-server >/dev/null 2>&1; then
      say "PASS   : openssh-server installed"
    else
      say "FAIL   : could not install openssh-server."
      say "         Check your network, then run: sudo apt install openssh-server"
      exit 1
    fi
  fi
fi

# -- 2. authorise the key -----------------------------------------------------
mkdir -p "$HOMEDIR/.ssh"
touch "$AUTH"

if grep -qF "$PUBKEY" "$AUTH" 2>/dev/null; then
  say "PASS   : key already authorised"
else
  echo "$PUBKEY" >> "$AUTH"
  say "PASS   : key added to $AUTH"
fi

#
# sshd silently ignores authorized_keys if the permissions are loose, and gives
# no useful error to the client -- just "Permission denied (publickey)".
#
chown -R "$WHO" "$HOMEDIR/.ssh"
chmod 700 "$HOMEDIR/.ssh"
chmod 600 "$AUTH"
say "PASS   : ownership and permissions set"

# -- 3. key-only login --------------------------------------------------------
#
# A drop-in rather than an edit of /etc/ssh/sshd_config, so the whole change is
# one deletable file and the distro's own config is untouched.
#
mkdir -p /etc/ssh/sshd_config.d
{
  echo "# Written by enable-ssh.sh for the mmiotrace work."
  echo "# Remove with:  sudo sh enable-ssh.sh --undo"
  echo "PubkeyAuthentication yes"
  echo "PasswordAuthentication no"
  echo "PermitRootLogin no"
} > "$DROPIN"
say "PASS   : wrote $DROPIN (key-only, no password login)"

if sshd -t 2>/dev/null; then
  say "PASS   : sshd config is valid"
else
  say "FAIL   : sshd rejected the config:"
  sshd -t
  rm -f "$DROPIN"
  say "         drop-in removed, nothing changed"
  exit 1
fi

# -- 4. start it --------------------------------------------------------------
systemctl enable ssh >/dev/null 2>&1 || systemctl enable sshd >/dev/null 2>&1
if systemctl restart ssh 2>/dev/null || systemctl restart sshd 2>/dev/null; then
  say "PASS   : sshd running and enabled at boot"
else
  say "FAIL   : sshd would not start -- run 'systemctl status ssh' for why"
  exit 1
fi

# -- 5. firewall --------------------------------------------------------------
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
  ufw allow ssh >/dev/null 2>&1 \
    && say "PASS   : ufw is active, port 22 allowed" \
    || say "WARN   : ufw is active but 'ufw allow ssh' failed"
else
  say "note   : ufw inactive or absent, no firewall change needed"
fi

# -- 6. tell the user where to connect ----------------------------------------
IP=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1)
HOSTN=$(hostname 2>/dev/null)

say ""
say "------------------------------------------------------------------"
say "Done. On the WINDOWS machine, run:"
say ""
if [ -n "$IP" ]; then
  say "    ssh $WHO@$IP"
else
  say "    ssh $WHO@<this-laptop-ip>"
  say "    (could not detect an IP -- is this laptop on the network?)"
fi
say ""
if [ -n "$HOSTN" ]; then
  say "If that IP changes, this also usually works:"
  say ""
  say "    ssh $WHO@$HOSTN.local"
  say ""
fi
say "Say yes at the fingerprint prompt on the first connection."
say ""
say "IMPORTANT -- sudo over SSH needs a password, and there is no terminal"
say "to type it into non-interactively. To let the capture run unattended:"
say ""
say "    echo '$WHO ALL=(ALL) NOPASSWD: ALL' | sudo tee /etc/sudoers.d/99-$WHO"
say "    sudo chmod 440 /etc/sudoers.d/99-$WHO"
say ""
say "That is a real reduction in security on this machine: anyone who gets a"
say "shell as $WHO becomes root without a password. It is reasonable for a"
say "laptop being used as a lab machine on a home network. Undo it with:"
say ""
say "    sudo rm /etc/sudoers.d/99-$WHO"
say ""
say "Revoke SSH access entirely when finished:"
say ""
say "    sudo sh enable-ssh.sh --undo"
say "------------------------------------------------------------------"
