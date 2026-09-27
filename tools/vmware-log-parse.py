#!/usr/bin/env python
"""vmware-log-parse.py -- turn a vmware.log into a timeline of the events this
project actually cares about, so nobody has to eyeball 170KB of log again.

Why this exists
---------------
Every diagnosis in this project so far has hinged on a handful of lines buried in
`C:\\Users\\Kentl\\macos-vm\\sonoma\\vmware.log`: a Tools heartbeat timeout, a
`GuestRpcSendTimedOut` naming *which* channel, a
`VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED`, a `guestInfo.detailed.data` write-back.
Grepping for them one pattern at a time loses the thing that actually matters --
their ORDER relative to each other and to power-on.

What it will and will not tell you
----------------------------------
This is a LOG READER. It reports what the vmx process wrote. That makes it
host-side evidence about the host's view of the guest, and nothing more:

  * A heartbeat timeout is the host observing silence on the toolbox channel. It
    is not proof the guest is hung -- see the one-directional-evidence rule.
  * `VIX_E_TOOLS_NOT_RUNNING` is what the host reports when a GuestRpc send
    times out. It is a channel verdict, not an installed-or-not verdict.
  * `guestInfo.detailed.data` IS guest-originated: the guest toolbox had to
    speak to produce it. That is the one positive datum in here.
  * `VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED` is an AUTH gate, not a dead
    channel. The two are constantly confused; this tool labels them apart.

Nothing here can establish that input reached the guest. Only a framebuffer
delta can do that -- use tools/png-delta.py for it.

Usage
-----
    python tools/vmware-log-parse.py [LOG] [options]

    LOG defaults to C:/Users/Kentl/macos-vm/sonoma/vmware.log

    --json              machine-readable output
    --since TS          only events at/after this ISO timestamp prefix
                        (e.g. --since 2026-08-07T22:39)
    --last-boot         only events from the most recent power-on onward
    --only KIND,KIND    restrict to these event kinds (see --list-kinds)
    --list-kinds        print the event kinds this parser knows and exit
    --no-collapse       do not collapse runs of identical repeated events
    --context N         with --collapse off, nothing; reserved for symmetry

Exit status
-----------
    0  parsed successfully (even if zero events matched)
    2  log file missing or unreadable
    3  bad arguments
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

DEFAULT_LOG = r"C:\Users\Kentl\macos-vm\sonoma\vmware.log"

# Every vmware.log line this project cares about starts with an ISO-ish stamp:
#   2026-08-07T22:39:24.402Z In(05) vmx Tools: sending 'OS_PowerOn' ...
LINE_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)\s+"
    r"(?P<level>\S+)\s+(?P<who>\S+)\s+(?P<msg>.*)$"
)

# ---------------------------------------------------------------------------
# Event catalogue.
#
# Each entry: (kind, severity, compiled regex, human label template).
# `severity` is one of: good, info, warn, bad, gate.
#   good -- positive evidence, something actually worked
#   gate -- an authorization/human gate, NOT a malfunction
#   bad  -- a real failure of the thing being attempted
#   warn -- noteworthy but expected/benign in isolation
#   info -- context anchor (power-on, config write)
#
# The regexes below were written against real lines in this project's log; every
# one of them has at least one confirmed hit. Do not "tidy" them without
# re-checking against the log.
# ---------------------------------------------------------------------------
EVENTS = [
    (
        "power_on",
        "info",
        re.compile(r"Tools: sending 'OS_PowerOn'"),
        "guest power-on signalled to Tools",
    ),
    (
        "hostname",
        "info",
        re.compile(r"^Hostname=(?P<host>\S+)"),
        "host name recorded: {host}",
    ),
    (
        "tools_running_status",
        "info",
        re.compile(
            r"Tools: \[RunningStatus\] Last heartbeat value (?P<val>\d+) "
            r"\(last received (?P<age>\d+)s ago\)"
        ),
        "toolbox RunningStatus heartbeat #{val}, last seen {age}s ago",
    ),
    (
        "tools_app_status",
        "info",
        re.compile(
            r"Tools: \[AppStatus\] Last heartbeat value (?P<val>\d+) "
            r"\(last received (?P<age>\d+)s ago\)"
        ),
        "toolbox AppStatus heartbeat #{val}, last seen {age}s ago",
    ),
    (
        "heartbeat_timeout",
        "warn",
        re.compile(r"Tools: Tools heartbeat timeout\."),
        "HEARTBEAT TIMEOUT -- host saw silence on the toolbox channel "
        "(host-side observation, NOT proof the guest is hung)",
    ),
    (
        "tools_running_change",
        "info",
        re.compile(r"Tools: Changing running status: (?P<frm>\d+) => (?P<to>\d+)"),
        "Tools running status {frm} -> {to}",
    ),
    (
        "guestrpc_timeout",
        "bad",
        re.compile(
            r"GuestRpcSendTimedOut: message to (?P<chan>\S+) timed out\."
        ),
        "GuestRpc send TIMED OUT on channel '{chan}'",
    ),
    (
        "tools_not_running",
        "bad",
        re.compile(r"Error VIX_E_TOOLS_NOT_RUNNING in "),
        "VIX_E_TOOLS_NOT_RUNNING -- host's verdict after a GuestRpc timeout "
        "(a CHANNEL verdict, not an installed-or-not verdict)",
    ),
    (
        "anon_prohibited",
        "gate",
        re.compile(r"Error VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED in "),
        "VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED -- AUTH GATE: a guest op was "
        "attempted without -gu/-gp. This is NOT a dead Tools channel.",
    ),
    (
        "install_tools",
        "info",
        re.compile(
            r"VMAutomation_InstallTools: upgradeOnly = (?P<upgrade>\d+)"
        ),
        "InstallTools REQUESTED by the host (upgradeOnly={upgrade}) -- a "
        "request only; it does not mean an install ran in the guest",
    ),
    (
        "socket_reset",
        "warn",
        re.compile(
            r"SOCKET (?P<sock>\d+) \((?P<pid>\d+)\) recv error 10054"
        ),
        "VIX client socket {sock} reset by peer (10054) -- normally just a "
        "vmrun process exiting",
    ),
    (
        "vix_conn_error",
        "warn",
        re.compile(
            r"VMAutomation: Connection Error \((?P<code>\d+)\) on "
            r"connection (?P<conn>\d+)\."
        ),
        "VMAutomation connection error ({code}) on connection {conn}",
    ),
    (
        "guestinfo_writeback",
        "good",
        re.compile(r"ConfigDB: Setting guestInfo\.detailed\.data"),
        "GUEST-ORIGINATED: guestInfo.detailed.data written -- the guest "
        "toolbox had to speak for this to exist",
    ),
    (
        "guestinfo_header",
        "good",
        re.compile(
            r"GuestInfo: HostinfoDetailedDataHeader version: (?P<ver>\d+)"
        ),
        "guest detailed-data header v{ver} received from the guest",
    ),
    (
        "guestinfo_ip",
        "good",
        re.compile(r"ConfigDB: Setting guestInfo\.ip\s*=\s*(?P<ip>\S+)"),
        "GUEST-ORIGINATED: guestInfo.ip = {ip}",
    ),
    (
        "tools_install_state",
        "info",
        re.compile(r"Tools_SetGuestInstallState|noTools, install possible"),
        "Tools install-state note from the vmx",
    ),
    (
        "cdrom_lock",
        "gate",
        re.compile(r"21004|cannot be (?:changed|disconnected) while the "
                   r"(?:virtual machine|guest) is"),
        "CD/DVD device change refused (21004-class) -- the GUEST holds the "
        "CD; eject from inside macOS first",
    ),
]

# Measured on this project's log: 47 socket_reset + 51 vix_conn_error out of 146
# events, and they ALTERNATE (one pair per vmrun invocation). Collapsing only
# consecutive *same-kind* events therefore does nothing for them -- verified by
# running it. Treat the pair as one noise class so it actually collapses.
NOISE_KINDS = {"socket_reset", "vix_conn_error"}

# collapse() rewrites a merged run of NOISE_KINDS into this SYNTHETIC kind, so it
# never appears in EVENTS (it has no regex -- nothing in the log says it). It is
# still a kind a caller can legitimately ask for with --only, so it has to be in
# the known-kind set and in --list-kinds, or the tool rejects a name it printed
# itself. Measured on the live log: 47 rows carry this kind, collapsed from
# 47 socket_reset + 51 vix_conn_error.
SYNTHETIC_KINDS = {
    "vmrun_client_churn": (
        "noise",
        "HOST-SIDE ONLY: collapsed run of vmrun's own VIX client sockets "
        "closing as each vmrun process exits -- NOT a guest failure",
    ),
}

# A synthetic kind cannot be filtered for directly: --only is applied inside
# parse() (the `if only:` block), which runs BEFORE collapse() rewrites any
# event's kind. At filter time NO event carries "vmrun_client_churn", so asking
# for it matched nothing and printed "(no matching events)" against a log that
# demonstrably contains 47 such rows. Registering the name in SYNTHETIC_KINDS
# only removed the spurious exit-3; it replaced it with a silent empty result,
# which is worse. The fix is to translate the synthetic name into the real kinds
# collapse() builds it from, and let collapse() re-derive the name afterwards.
SYNTHETIC_SOURCES = {
    "vmrun_client_churn": NOISE_KINDS,
}

SEV_ORDER = {"bad": 0, "gate": 1, "good": 2, "warn": 3, "info": 4}
SEV_MARK = {
    "good": "[+]",
    "bad": "[!]",
    "gate": "[G]",
    "warn": "[~]",
    "info": "[ ]",
}


def parse(path, since=None, last_boot=False, only=None):
    """Return (events, stats). Each event is a dict; never raises on odd lines."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.readlines()
    except OSError as exc:
        print("[FAIL] cannot read %s: %s" % (path, exc), file=sys.stderr)
        return None, None

    events = []
    for idx, line in enumerate(raw, start=1):
        m = LINE_RE.match(line.rstrip("\n"))
        if not m:
            continue
        ts = m.group("ts")
        msg = m.group("msg")
        for kind, sev, rx, label in EVENTS:
            hit = rx.search(msg)
            if not hit:
                continue
            fields = hit.groupdict()
            try:
                text = label.format(**fields)
            except (KeyError, IndexError):
                text = label
            events.append(
                {
                    "line": idx,
                    "ts": ts,
                    "who": m.group("who"),
                    "kind": kind,
                    "severity": sev,
                    "label": text,
                    "fields": fields,
                    "raw": msg.strip(),
                }
            )
            break  # first matching rule wins; catalogue order is the priority

    stats = {"lines_total": len(raw), "events_total": len(events)}

    if last_boot:
        boots = [e for e in events if e["kind"] == "power_on"]
        if boots:
            cut = boots[-1]["line"]
            events = [e for e in events if e["line"] >= cut]
            stats["last_boot_line"] = cut
            stats["last_boot_ts"] = boots[-1]["ts"]
        else:
            stats["last_boot_line"] = None

    if since:
        events = [e for e in events if e["ts"] >= since]

    if only:
        want = set(only)
        events = [e for e in events if e["kind"] in want]

    stats["events_shown"] = len(events)
    return events, stats


def collapse(events):
    """Collapse consecutive same-kind events into one row with a count.

    Two things collapse: runs of the identical kind, and runs drawn from
    NOISE_KINDS (the socket_reset/vix_conn_error pairs vmrun emits on every
    invocation, which alternate and so would otherwise never merge). First and
    last line numbers are preserved so you can always go back to the raw log.
    """
    out = []
    for ev in events:
        prev = out[-1] if out else None
        same = prev is not None and prev["kind"] == ev["kind"]
        both_noise = (
            prev is not None
            and prev["kind"] in NOISE_KINDS
            and ev["kind"] in NOISE_KINDS
        )
        if same or both_noise:
            prev["count"] += 1
            prev["ts_last"] = ev["ts"]
            prev["line_last"] = ev["line"]
            if both_noise and not same:
                prev["kind"] = "vmrun_client_churn"
                prev["severity"] = "warn"
                prev["label"] = (
                    "vmrun client connect/disconnect churn (socket 10054 + "
                    "VMAutomation connection error) -- one pair per vmrun "
                    "call, benign on its own"
                )
            continue
        row = dict(ev)
        row["count"] = 1
        row["ts_last"] = ev["ts"]
        row["line_last"] = ev["line"]
        out.append(row)
    return out


def summarize(events):
    """Counts per kind, plus the few conclusions this log CAN support."""
    per_kind = {}
    for ev in events:
        per_kind[ev["kind"]] = per_kind.get(ev["kind"], 0) + 1

    findings = []

    if per_kind.get("guestinfo_writeback") or per_kind.get("guestinfo_header"):
        findings.append(
            ("good", "Toolbox channel HAS carried guest-originated data "
                     "(guestInfo write-back). Tools are installed and were "
                     "talking at that timestamp.")
        )
    if per_kind.get("anon_prohibited"):
        findings.append(
            ("gate", "At least one guest op was attempted ANONYMOUSLY and "
                     "refused. This is an auth gate: supply -gu/-gp. It says "
                     "nothing about Tools health.")
        )
    if per_kind.get("tools_not_running") and (
        per_kind.get("guestinfo_writeback") or per_kind.get("guestinfo_header")
    ):
        findings.append(
            ("warn", "VIX_E_TOOLS_NOT_RUNNING appears in the SAME log as a "
                     "guest write-back. Both are true: the channel went quiet "
                     "for one send, and Tools are nonetheless installed. Do "
                     "not read the error as 'Tools absent'.")
        )
    elif per_kind.get("tools_not_running"):
        findings.append(
            ("bad", "VIX_E_TOOLS_NOT_RUNNING with no guest write-back in the "
                    "same window. Channel unproven in both directions.")
        )
    chans = sorted(
        {
            ev["fields"].get("chan")
            for ev in events
            if ev["kind"] == "guestrpc_timeout" and ev["fields"].get("chan")
        }
    )
    if chans:
        findings.append(
            ("bad", "GuestRpc timeouts seen on channel(s): %s. 'toolbox-dnd' "
                    "is drag-and-drop and is largely harmless; 'toolbox' is "
                    "the one that carries guest ops." % ", ".join(chans))
        )
    if per_kind.get("install_tools"):
        findings.append(
            ("info", "InstallTools was REQUESTED %d time(s) by the host. A "
                     "request is not an install -- confirm in the guest."
             % per_kind["install_tools"])
        )
    if per_kind.get("cdrom_lock"):
        findings.append(
            ("gate", "A CD/DVD change was refused because the guest holds the "
                     "device. Eject inside macOS, then retry.")
        )
    if not findings and not per_kind:
        findings.append(
            ("info", "No events of interest matched. That is an absence, not "
                     "a clean bill of health.")
        )
    elif not findings:
        # Rows WERE shown -- they just aren't any of the kinds a finding rule
        # keys on (this is the normal case for a noise-only --only filter).
        # The old text claimed "No events of interest matched" here, which
        # printed directly under a "98 shown" header and contradicted it.
        findings.append(
            ("info", "%d event(s) matched but none of them trigger a finding "
                     "rule -- the kinds shown (%s) are timeline context only. "
                     "Absence of a finding here is a consequence of the "
                     "--only filter, not a clean bill of health."
             % (sum(per_kind.values()), ", ".join(sorted(per_kind))))
        )
    return per_kind, findings


def render(events, stats, per_kind, findings, path, collapsed,
           synth_asked=None):
    print("vmware.log timeline -- %s" % path)
    print("  %d lines scanned, %d event(s) of interest, %d shown"
          % (stats["lines_total"], stats["events_total"],
             stats["events_shown"]))
    if stats.get("last_boot_ts"):
        print("  last power-on: %s (line %d)"
              % (stats["last_boot_ts"], stats["last_boot_line"]))
    for kind, src in sorted((synth_asked or {}).items()):
        print("  note: '%s' is synthetic -- filtered as %s, then re-collapsed "
              "under that name" % (kind, "+".join(src)))
        if not collapsed:
            print("        with --no-collapse the synthetic name is never "
                  "minted; rows appear as their real kinds")
    print("")

    if not events:
        print("  (no matching events)")
    else:
        print("TIMELINE")
        for ev in events:
            mark = SEV_MARK.get(ev["severity"], "[ ]")
            if collapsed and ev["count"] > 1:
                span = "L%d-%d x%d" % (ev["line"], ev["line_last"],
                                       ev["count"])
                stamp = "%s..%s" % (ev["ts"][11:23], ev["ts_last"][11:23])
            else:
                span = "L%d" % ev["line"]
                stamp = ev["ts"][11:23]
            print("  %s %-25s %-16s %s" % (mark, stamp, span, ev["label"]))

    print("")
    print("COUNTS")
    for kind in sorted(per_kind, key=lambda k: (-per_kind[k], k)):
        print("  %-24s %d" % (kind, per_kind[kind]))

    print("")
    print("WHAT THIS LOG CAN AND CANNOT SUPPORT")
    for sev, text in findings:
        print("  %s %s" % (SEV_MARK.get(sev, "[ ]"), text))
    print("")
    print("  Reminder: everything above is the HOST's view. No line in a "
          "vmware.log")
    print("  can establish that injected input reached the guest -- only a "
          "framebuffer")
    print("  delta can (tools/png-delta.py A.png B.png --floor CONTROL.png).")


def main(argv):
    ap = argparse.ArgumentParser(
        prog="vmware-log-parse.py",
        description="Timeline the events this project cares about out of a "
                    "vmware.log.",
    )
    ap.add_argument("log", nargs="?", default=DEFAULT_LOG)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--since", metavar="TS")
    ap.add_argument("--last-boot", action="store_true")
    ap.add_argument("--only", metavar="KIND,KIND")
    ap.add_argument("--list-kinds", action="store_true")
    ap.add_argument("--no-collapse", action="store_true")
    args = ap.parse_args(argv)

    if args.list_kinds:
        for kind, sev, _rx, label in EVENTS:
            print("%-24s %-5s %s" % (kind, sev, label.split(" -- ")[0]))
        for kind, (sev, label) in sorted(SYNTHETIC_KINDS.items()):
            print("%-24s %-5s %s  (synthetic: produced by collapse, not "
                  "matched in the log)" % (kind, sev, label.split(" -- ")[0]))
        return 0

    only = None
    synth_asked = {}
    if args.only:
        only = [k.strip() for k in args.only.split(",") if k.strip()]
        known = {k for k, _s, _r, _l in EVENTS} | set(SYNTHETIC_KINDS)
        bad = [k for k in only if k not in known]
        if bad:
            print("[FAIL] unknown kind(s): %s (see --list-kinds)"
                  % ", ".join(bad), file=sys.stderr)
            return 3
        # Translate synthetic kinds into the kinds collapse() derives them from,
        # because the filter runs before collapse(). Keep a record so the output
        # can say so out loud -- COUNTS is computed pre-collapse and will list
        # the constituent kinds, not the synthetic one.
        expanded = []
        for kind in only:
            src = SYNTHETIC_SOURCES.get(kind)
            if src:
                synth_asked[kind] = sorted(src)
                expanded.extend(sorted(src))
            else:
                expanded.append(kind)
        only = sorted(set(expanded))

    if not os.path.exists(args.log):
        print("[FAIL] no such log: %s" % args.log, file=sys.stderr)
        return 2

    events, stats = parse(args.log, since=args.since,
                          last_boot=args.last_boot, only=only)
    if events is None:
        return 2

    collapsed = not args.no_collapse
    per_kind, findings = summarize(events)
    shown = collapse(events) if collapsed else events

    if args.json:
        print(json.dumps(
            {
                "log": args.log,
                "stats": stats,
                "counts": per_kind,
                "findings": [{"severity": s, "text": t} for s, t in findings],
                "synthetic_expanded": synth_asked,
                "events": shown,
            },
            indent=2,
        ))
        return 0

    render(shown, stats, per_kind, findings, args.log, collapsed,
           synth_asked=synth_asked)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
