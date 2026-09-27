#!/usr/bin/env python3
"""
check-kext-personalities.py -- every IOPCIMatch in phase0-kext/Info.plist must
name a device ID that a vendored device-ID document attributes to the die this
project targets on that vendor's track.

Why this exists
---------------
A personality that matches the WRONG device is not a build error and not a parse
error. The kext compiles, installs, loads, and then simply never probes, because
IOKit never hands it a provider. On the operator's screen that is indistinguisable
from "no GPU found", from a broken probe(), from SIP, and from an unsigned-kext
refusal -- and it is the one cause that costs a power cycle to even look at. It is
also a one-character mistake: 0x25a210de and 0x25a110de differ by a nibble, and
nothing in the build or in tests/ reads this file at all.

The file is also a plist with hand-written XML comments, which have their own
silent failure: XML forbids ``--`` inside a comment, and plutil/plistlib reject
the WHOLE file with an error that does not name the offending line. Info.plist
carries a warning about that in its own prose. Parsing the file properly here (via
plistlib, not a regex) makes that a red gate on the host instead of a surprise on
the target.

What is derived rather than typed
--------------------------------
The accepted device IDs come from parsing the markdown table in each track's
document -- DEVICE-IDS.md for NVIDIA, transcribed from NVIDIA's own shipped
``g_nv_name_released.h``, and INTEL-DEVICE-IDS.md for Intel, transcribed from the
i915 table vendored under ``reference/i915/``. Typing the IDs here would create a
second table nothing keeps true -- and this project has already been bitten by
exactly that (a "measured" count table that was false on the day it was written).

The target die names are not typed as bare fact either: each active track must be
corroborated by CLAUDE.md naming the die or one of its aliases. If the project
ever re-targets, that assertion goes red and points at the disagreement rather
than silently validating against the old die.

The alias list is load-bearing rather than decorative. CLAUDE.md describes the
Intel silicon as "Tiger Lake" and "Gen12" and never spells the bare token TGL, so
a literal die-name search would report a disagreement against a document that
does in fact describe the silicon.

What each verdict means
-----------------------
  OK    the token decodes to a vendor this project has a track for, and to a
        device ID that track's document lists under the track's die.
  FAIL  anything else, including four cases that each look harmless:
          * a device ID that is real silicon but the WRONG die (0x2523 is a
            GA106 RTX 3050 Ti -- in the table, right vendor, right marketing
            name, wrong architecture, different register headers).
          * a vendor-wide wildcard (device half 0x0000). That is what this
            personality started as, and it claims every card from that vendor in
            the machine -- including on a host that is not the target laptop.
          * a swapped pair. IOPCIMatch is (device << 16) | vendor, so writing
            it the other way round produces a plausible-looking hex string that
            matches nothing.
          * a vendor this project HAS a track for, whose document is not in the
            tree. That is not "no opinion" -- it is a personality nothing can
            vouch for, and it must not pass by default.

Vacuity refusals
----------------
Zero personalities, zero match tokens, no track document present at all, or a
table that yields fewer IDs than its track's floor are all FAILures rather than a
quiet pass. A checker that verifies nothing prints the same success line as one
that verified everything, so the absence of anything to check is itself the
defect.

Usage
-----
    python tools/check-kext-personalities.py [REPO_ROOT]

Exit 0 if every personality is accounted for, 1 otherwise.
"""

import plistlib
import re
import sys
from pathlib import Path

#
# THE TWO TRACKS.
#
# This checker used to hold ONE die and ONE vendor as module constants and loop
# over every personality with them. That was correct while the plist held only
# NV3050Probe. It stops being correct the moment an Intel personality appears:
# the vendor branch would fire with "vendor half is 0x8086, not 0x10DE" -- a
# message that is right about its own arithmetic and wrong about the world, the
# exact failure shape handoff16 SS1 records for the IG_PIPE_ prefix collision.
#
# So a personality is now classified against the track its vendor half NAMES,
# and each track carries its own accept-set document. A token whose vendor is
# neither track's is still rejected, and the message still names the vendor,
# because a swapped (device << 16) | vendor pair is a valid 32-bit number that
# matches nothing and reads as fine until it is split.
#
# `doc` is the file the accept set is PARSED from -- this checker refuses to
# hold its own accept set, because a hardcoded set is a second table that
# nothing keeps true. `aliases` exists because CLAUDE.md corroborates the target
# in prose: it says "Tiger Lake" and "Gen12", never the bare token "TGL", so a
# literal die-name search would fail against a document that DOES describe the
# silicon.
#
TRACKS = (
    {
        "vendor": 0x10DE,           # NVIDIA. A constant of the world.
        "vendor_name": "NVIDIA",
        "die": "GA107",
        "doc": "DEVICE-IDS.md",
        "aliases": ("GA107",),
        "min_ids": 4,
    },
    {
        "vendor": 0x8086,           # Intel. Likewise.
        "vendor_name": "Intel",
        "die": "TGL",
        "doc": "INTEL-DEVICE-IDS.md",
        "aliases": ("TGL", "Tiger Lake", "TigerLake"),
        "min_ids": 4,
    },
)

TRACK_BY_VENDOR = {track["vendor"]: track for track in TRACKS}

# Keys IOKit will actually match on. Either is legitimate; at least one must be
# present, or the personality can never be selected.
MATCH_KEYS = ("IOPCIMatch", "IOPCIPrimaryMatch")

# | Marketing name | `0x25A2`, `0x25A5` | GA107 | Ampere |
TABLE_ROW_RE = re.compile(r"^\s*\|(?P<name>[^|]*)\|(?P<ids>[^|]*)\|"
                          r"(?P<die>[^|]*)\|(?P<arch>[^|]*)\|\s*$")
HEX16_RE = re.compile(r"0x([0-9A-Fa-f]{4})\b")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def ids_by_die(doc: str):
    """{die: {device_id, ...}} parsed out of a device-ID document's table.

    Separator rows (``|---|---|``) and the header row carry no 0xNNNN tokens, so
    they fall out naturally rather than needing to be recognised.
    """
    table = {}
    for line in doc.splitlines():
        match = TABLE_ROW_RE.match(line)
        if match is None:
            continue
        ids = {int(h, 16) for h in HEX16_RE.findall(match.group("ids"))}
        if not ids:
            continue
        die = match.group("die").strip().strip("*` ").upper()
        if not die:
            continue
        table.setdefault(die, set()).update(ids)
    return table


def split_match_tokens(value: str):
    """The individual match tokens in one IOPCIMatch string.

    IOKit accepts whitespace-separated alternatives, and each may carry an
    ``&mask`` suffix. The mask is kept so a wildcard cannot hide behind one.
    """
    return [tok for tok in value.replace(",", " ").split() if tok.strip()]


def decode_token(token: str):
    """(device, vendor, mask, error) for one IOPCIMatch token.

    IOPCIMatch encodes (device << 16) | vendor. Reporting device and vendor
    separately is deliberate: a swapped pair is a valid 32-bit number and reads
    as fine until it is split.
    """
    body, _, mask_text = token.partition("&")
    try:
        value = int(body, 16)
    except ValueError:
        return None, None, None, f"{token!r} is not a hexadecimal match value"
    if value < 0 or value > 0xFFFFFFFF:
        return None, None, None, f"{token!r} is not a 32-bit match value"

    mask = None
    if mask_text:
        try:
            mask = int(mask_text, 16)
        except ValueError:
            return None, None, None, f"{token!r} has an unparseable &mask"

    return (value >> 16) & 0xFFFF, value & 0xFFFF, mask, None


def known_vendor_list() -> str:
    """"0x10DE (NVIDIA) or 0x8086 (Intel)" -- built from TRACKS, never typed."""
    return " or ".join(f"0x{t['vendor']:04X} ({t['vendor_name']})" for t in TRACKS)


def classify_token(token: str, states):
    """("OK"|"FAIL", reason) for one match token.

    ``states`` maps a vendor ID to that track's loaded state. The token is
    classified against the track its OWN vendor half names -- not against a
    single module-level target -- because a checker that assumed one vendor
    would reject a correct Intel personality with an arithmetically correct
    message about NVIDIA.
    """
    device, vendor, mask, error = decode_token(token)
    if error is not None:
        return "FAIL", error

    track = TRACK_BY_VENDOR.get(vendor)
    if track is None:
        return "FAIL", (f"vendor half is 0x{vendor:04X}, which is neither "
                        f"{known_vendor_list()} -- IOPCIMatch is "
                        f"(device << 16) | vendor, so a swapped pair looks like "
                        f"a valid number and matches nothing")

    state = states.get(vendor)
    if state is None:
        return "FAIL", (f"vendor half is 0x{vendor:04X} ({track['vendor_name']}), "
                        f"but {track['doc']} is not in this tree -- nothing "
                        f"attributes device 0x{device:04X} to {track['die']}, and "
                        f"a personality no document vouches for must not pass by "
                        f"default")

    if device == 0x0000:
        return "FAIL", (f"the device half is 0x0000, i.e. a vendor-wide wildcard "
                        f"-- this claims EVERY {track['vendor_name']} card, "
                        f"including on a machine that is not the target laptop")

    if mask is not None and (mask & 0xFFFF0000) != 0xFFFF0000:
        return "FAIL", (f"&0x{mask:08X} does not fully constrain the device half, "
                        f"so this matches more devices than 0x{device:04X}")

    if device in state["accepted"]:
        return "OK", (f"device 0x{device:04X} is a {track['die']} ID in "
                      f"{track['doc']}")

    other = sorted(die for die, ids in state["known"].items() if device in ids)
    if other:
        return "FAIL", (f"device 0x{device:04X} is real silicon, but "
                        f"{track['doc']} attributes it to {', '.join(other)}, "
                        f"not {track['die']} -- different architecture and "
                        f"different register headers, and the marketing name is "
                        f"the same")

    return "FAIL", (f"device 0x{device:04X} appears nowhere in {track['doc']}, so "
                    f"nothing says this is {track['die']} silicon at all")


def load_plist(path: Path):
    """(plist, None) or (None, message).

    A parse failure is reported with the ``--`` hazard named, because that is the
    cause here and expat's own message does not name the line.
    """
    try:
        with open(path, "rb") as handle:
            return plistlib.load(handle), None
    except Exception as exc:                     # noqa: BLE001 -- report, never crash
        hint = ""
        if "--" in read_text(path):
            hint = ("; the file contains a double hyphen, which XML forbids inside "
                    "a comment and which rejects the WHOLE plist without naming "
                    "the line")
        return None, f"{path.name} does not parse as a plist: {exc}{hint}"


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else \
        Path(__file__).resolve().parent.parent

    plist_path = root / "phase0-kext" / "Info.plist"
    claude_doc = root / "CLAUDE.md"

    failures = []

    if not plist_path.is_file():
        print(f"FAIL: {plist_path} does not exist")
        return 1

    #
    # A track is ACTIVE when its document is in the tree. An absent document is
    # not fatal on its own -- this checker has to stay runnable on a tree that
    # carries only one of the two tracks -- but it is fatal for any personality
    # that names that vendor, which classify_token() handles. What IS fatal here
    # is no document at all: that is the vacuous configuration where every
    # possible token would be rejected for the same reason and the checker would
    # be measuring its own emptiness.
    #
    states = {}
    for track in TRACKS:
        doc_path = root / track["doc"]
        if not doc_path.is_file():
            continue
        known = ids_by_die(read_text(doc_path))
        states[track["vendor"]] = {
            "track": track,
            "known": known,
            "accepted": known.get(track["die"].upper(), set()),
        }

    if not states:
        print("FAIL: none of " + ", ".join(t["doc"] for t in TRACKS) + " exists "
              "-- the accepted device IDs are derived from them, so there is "
              "nothing to check against")
        return 1

    # The target dies must be documented, not asserted here on trust.
    if not claude_doc.is_file():
        print(f"FAIL: {claude_doc} does not exist -- nothing corroborates "
              + ", ".join(s["track"]["die"] for s in states.values())
              + " as a target die")
        return 1

    claude_text = read_text(claude_doc)
    for state in states.values():
        track = state["track"]
        if not any(alias in claude_text for alias in track["aliases"]):
            print(f"FAIL: CLAUDE.md mentions none of "
                  f"{', '.join(track['aliases'])}, so this checker would be "
                  f"validating Info.plist against silicon the project no longer "
                  f"targets")
            return 1

        if len(state["accepted"]) < track["min_ids"]:
            print(f"FAIL: parsed only {len(state['accepted'])} {track['die']} "
                  f"device ID(s) out of {track['doc']} (floor "
                  f"{track['min_ids']}) -- the table parse is broken, not the "
                  f"plist; a near-empty accept set would reject a correct "
                  f"Info.plist")
            return 1

    plist, error = load_plist(plist_path)
    if error is not None:
        print(f"FAIL: {error}")
        return 1

    personalities = plist.get("IOKitPersonalities")
    if not isinstance(personalities, dict) or not personalities:
        print("FAIL: Info.plist declares no IOKitPersonalities -- the kext would "
              "load and never be matched against anything")
        return 1

    for state in states.values():
        track = state["track"]
        print(f"accepted {track['die']} device IDs from {track['doc']}: "
              + ", ".join(f"0x{i:04X}" for i in sorted(state["accepted"])))

    checked_tokens = 0
    for name in sorted(personalities):
        body = personalities[name]
        if not isinstance(body, dict):
            failures.append(f"personality {name} is not a dictionary")
            continue

        provider = body.get("IOProviderClass")
        if provider != "IOPCIDevice":
            failures.append(f"{name}: IOProviderClass is {provider!r}, not "
                            f"'IOPCIDevice' -- an IOPCIMatch is only consulted "
                            f"for a PCI provider, so the match string would be "
                            f"inert")

        if not body.get("IOClass"):
            failures.append(f"{name}: no IOClass, so nothing would be "
                            f"instantiated even on a match")

        present = [k for k in MATCH_KEYS if body.get(k)]
        if not present:
            failures.append(f"{name}: no {' or '.join(MATCH_KEYS)} -- this "
                            f"personality can never be selected")
            continue

        for key in present:
            value = body[key]
            if not isinstance(value, str):
                failures.append(f"{name}.{key} is {type(value).__name__}, not a "
                                f"string")
                continue
            tokens = split_match_tokens(value)
            if not tokens:
                failures.append(f"{name}.{key} is empty")
                continue
            for token in tokens:
                verdict, reason = classify_token(token, states)
                checked_tokens += 1
                print(f"  {verdict:<4} {name}.{key} {token} -- {reason}")
                if verdict != "OK":
                    failures.append(f"{name}.{key} {token}: {reason}")

    #
    # Order matters here, and it was wrong once. The vacuity check used to run
    # FIRST, so a personality with no match key at all -- already diagnosed by
    # name in `failures` -- was reported as "extraction is broken" instead. That
    # is red for a true reason phrased as a false one: it sends the reader to
    # this checker's regexes rather than to the missing IOPCIMatch. Specific
    # diagnoses win; the vacuity refusal is only correct when nothing else
    # explains the silence.
    #
    if failures:
        print()
        print(f"check-kext-personalities: {len(failures)} problem(s)")
        for item in failures:
            print(f"  {item}")
        return 1

    if checked_tokens == 0:
        print("FAIL: no match tokens were examined at all -- extraction is "
              "broken, not the plist")
        return 1

    print(f"check-kext-personalities: {len(personalities)} personality(ies), "
          f"{checked_tokens} match token(s), all naming documented target "
          f"silicon")
    return 0


if __name__ == "__main__":
    sys.exit(main())
