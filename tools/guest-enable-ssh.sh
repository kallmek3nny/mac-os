#!/bin/sh
#
# guest-enable-ssh.sh -- turn on Remote Login (sshd) INSIDE the macOS Sonoma
# guest, so that every later session gets a real shell instead of injecting
# scancodes at a screenshot.
#
# WHERE THIS RUNS
# ---------------
# This runs in the GUEST, not on the Windows host. Do not confuse it with
# ../enable-ssh.sh, which is the mirror-image script for the Linux laptop.
#
# There are two ways to get it into the guest, in order of preference:
#
#   1. VMware Tools shared folder / copyFileFromHostToGuest, once
#      `vmrun checkToolsState` reports `running` (task t-f8acfa). Then:
#
#          vmrun -gu <user> -gp <pass> copyFileFromHostToGuest \
#              C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx \
#              "D:\nerd shit\other projects\mac os\tools\guest-enable-ssh.sh" \
#              /tmp/guest-enable-ssh.sh
#          vmrun -gu <user> -gp <pass> runScriptInGuest \
#              C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx \
#              /bin/sh "sh /tmp/guest-enable-ssh.sh"
#
#   2. Typed into a guest Terminal with tools/vm-type.py, as a here-doc, when
#      Tools is unavailable. Slow and fragile; see TYPED FALLBACK below.
#
# WHAT IT DOES
# ------------
# Enables Remote Login, confirms sshd is actually LISTENING (not merely that a
# command returned 0), and prints the guest's own view of its address so the
# host can be told where to connect. It authorises a public key if one is
# supplied, so the host never needs the account password again.
#
# It is idempotent: safe to re-run. Nothing is removed, the key is appended
# only if absent.
#
# USAGE
# -----
#     sudo sh guest-enable-ssh.sh                 # password auth only
#     sudo sh guest-enable-ssh.sh "ssh-ed25519 AAAA... host-key"
#
# EXIT STATUS
# -----------
#     0   sshd is listening on 22 -- verified by a post-condition, not by a
#         successful call to the enable command
#     1   not run as root
#     2   enable command ran but nothing is listening on 22 (the interesting
#         failure: usually the Full Disk Access / TCC prompt on the console)
#     3   listening, but only on loopback -- the host still cannot reach it
#

set -u

AUTHKEY="${1:-}"

say() { printf '%s\n' "$*"; }
rule() { say '--------------------------------------------------------------'; }

say "guest-enable-ssh.sh -- $(date -u '+%Y-%m-%dT%H:%M:%SZ') UTC"
say "host        : $(hostname 2>/dev/null || echo unknown)"
say "macOS       : $(sw_vers -productVersion 2>/dev/null || echo unknown) \
($(sw_vers -buildVersion 2>/dev/null || echo unknown))"
say "invoked as  : uid=$(id -u) user=$(id -un)"
rule

if [ "$(id -u)" -ne 0 ]; then
	say "REFUSING: must run as root. Re-run:  sudo sh $0"
	exit 1
fi

# The account that owns the GUI session -- that is who the host will ssh in as,
# and whose ~/.ssh must hold the key. $SUDO_USER is right under sudo; fall back
# to the console owner, which is what stat(1) on /dev/console reports.
TARGET_USER="${SUDO_USER:-}"
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
	TARGET_USER=$(stat -f '%Su' /dev/console 2>/dev/null || echo '')
fi
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
	say "WARNING: could not identify a non-root console user; using root."
	TARGET_USER=root
fi
TARGET_HOME=$(dscl . -read "/Users/$TARGET_USER" NFSHomeDirectory 2>/dev/null \
	| awk '{print $2}')
[ -n "$TARGET_HOME" ] || TARGET_HOME="/Users/$TARGET_USER"
say "ssh account : $TARGET_USER  (home $TARGET_HOME)"
rule

# --- 1. enable Remote Login -------------------------------------------------
#
# Two routes. `systemsetup -setremotelogin` is the documented one but on recent
# macOS it needs Full Disk Access for the calling binary and fails with a TCC
# error when run over a non-interactive channel. launchctl enabling the sshd
# job directly does not, so try it first and keep systemsetup as the fallback.

say "[1/4] enabling Remote Login"

launchctl enable system/com.openssh.sshd 2>&1 | sed 's/^/      launchctl: /'
launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist 2>&1 \
	| sed 's/^/      bootstrap: /'

# bootstrap returns 5 ("Input/output error") when the job is already loaded --
# that is success for our purposes, so its exit code is deliberately not tested.
# The post-condition below is the only thing that decides.

if ! launchctl print system/com.openssh.sshd >/dev/null 2>&1; then
	say "      launchctl route did not register the job; trying systemsetup"
	systemsetup -setremotelogin on 2>&1 | sed 's/^/      systemsetup: /'
fi

# --- 2. authorise the host key ----------------------------------------------

say "[2/4] authorising host public key"

if [ -n "$AUTHKEY" ]; then
	SSHDIR="$TARGET_HOME/.ssh"
	AUTHFILE="$SSHDIR/authorized_keys"
	mkdir -p "$SSHDIR"
	touch "$AUTHFILE"
	if grep -qF "$AUTHKEY" "$AUTHFILE" 2>/dev/null; then
		say "      key already present -- not appending again"
	else
		printf '%s\n' "$AUTHKEY" >> "$AUTHFILE"
		say "      key appended to $AUTHFILE"
	fi
	# sshd silently ignores a group/world-writable ~/.ssh. This is the single
	# most common reason key auth "just doesn't work" with no useful error.
	chmod 700 "$SSHDIR"
	chmod 600 "$AUTHFILE"
	chown -R "$TARGET_USER" "$SSHDIR"
	say "      perms: $(ls -ld "$SSHDIR" | awk '{print $1, $3}') / \
$(ls -l "$AUTHFILE" | awk '{print $1, $3}')"
else
	say "      no key argument given -- password auth only"
fi

# --- 3. make sure the account is allowed in ---------------------------------
#
# Remote Login can be on while the access list restricts it to an empty group,
# in which case sshd listens and every login is refused. Check explicitly.

say "[3/4] checking the Remote Login access list"

if dseditgroup -o read com.apple.access_ssh >/dev/null 2>&1; then
	say "      access group com.apple.access_ssh EXISTS (access is restricted)"
	if dseditgroup -o checkmember -m "$TARGET_USER" com.apple.access_ssh 2>&1 \
		| grep -q '^yes'; then
		say "      $TARGET_USER is a member -- ok"
	else
		say "      $TARGET_USER is NOT a member -- adding"
		dseditgroup -o edit -a "$TARGET_USER" -t user com.apple.access_ssh 2>&1 \
			| sed 's/^/      dseditgroup: /'
	fi
else
	say "      no access group -- all users permitted (macOS default)"
fi

# --- 4. post-condition: is anything actually LISTENING on 22? ---------------
#
# The whole point of this section. `systemsetup -setremotelogin on` returning 0
# does not mean sshd is up; on a TCC-blocked run it prints an error and still
# exits 0. A listening socket is the only evidence that counts.

say "[4/4] verifying the post-condition"
rule

LISTEN=''
for _try in 1 2 3 4 5 6 7 8 9 10; do
	LISTEN=$(netstat -an 2>/dev/null | grep -E '^tcp.*\.22[[:space:]]' \
		| grep -i listen)
	[ -n "$LISTEN" ] && break
	sleep 1
done

say "remote login (systemsetup reports):"
systemsetup -getremotelogin 2>&1 | sed 's/^/  /'

say "listening sockets on port 22:"
if [ -n "$LISTEN" ]; then
	printf '%s\n' "$LISTEN" | sed 's/^/  /'
else
	say "  (none)"
fi

say "launchd job:"
launchctl print system/com.openssh.sshd 2>&1 | head -n 6 | sed 's/^/  /'

say "guest addresses:"
ifconfig 2>/dev/null | grep 'inet ' | grep -v '127.0.0.1' | sed 's/^/  /'

rule

if [ -z "$LISTEN" ]; then
	say "RESULT: FAILED -- Remote Login was enabled but nothing is listening"
	say "        on port 22 after 10s."
	say ""
	say "        Most likely cause: the TCC / Full Disk Access prompt. Look at"
	say "        the guest console -- if a dialog is waiting, this script cannot"
	say "        dismiss it. Enable it by hand instead:"
	say "          System Settings > General > Sharing > Remote Login = ON"
	exit 2
fi

# Listening on loopback only is a real and non-obvious failure: from inside the
# guest everything looks perfect and the host still cannot connect.
if ! printf '%s\n' "$LISTEN" | grep -qE '(\*\.22|0\.0\.0\.0\.22)'; then
	say "RESULT: PARTIAL -- sshd is listening, but not on a wildcard address."
	say "        The host will not be able to reach it. Sockets found:"
	printf '%s\n' "$LISTEN" | sed 's/^/          /'
	exit 3
fi

GUESTIP=$(ifconfig 2>/dev/null | grep 'inet ' | grep -v '127.0.0.1' \
	| awk '{print $2}' | head -n 1)

say "RESULT: OK -- sshd is listening on all interfaces."
say ""
say "From the Windows host, connect with:"
say ""
say "    ssh $TARGET_USER@${GUESTIP:-<guest-ip>}"
say ""
say "Verify from the host BEFORE trusting this result -- a listening socket in"
say "the guest is not proof the host can route to it:"
say ""
say "    Test-NetConnection ${GUESTIP:-<guest-ip>} -Port 22"
say ""
exit 0
