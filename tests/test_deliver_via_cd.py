#!/usr/bin/env python3
"""
test_deliver_via_cd.py -- regression tests for ``tools/deliver-via-cd.py``.

Why this exists
---------------
The tool edits ``Sonoma.vmx``, which is the file that describes an installed
macOS system. Two of its checks are the load-bearing ones, and both fail
*silently* if they regress:

1. **It must refuse to repoint a disk.** ``sata0:0`` and ``sata0:1`` are both
   ``deviceType = "disk"`` carrying real VMDKs. Rewriting either ``fileName``
   to point at an ISO detaches the installed system, and the symptom appears at
   next power-on as an unbootable VM -- far from the tool that caused it.

2. **The edit must stay in the authored region.** This VMX has no labelled
   boundary: authored content ends at ``uuid.bios`` and VMware's runtime
   write-back begins at ``uuid.location``. An appended key lands in VMware's
   region and is silently discarded, which looks exactly like a guest that
   ignored the disc.

The third thing worth pinning is the Joliet name. Without it the guest sees the
8.3-mangled ``NV3050PR.TAR`` instead of ``NV3050Probe.kext.tar``, and again the
failure surfaces inside the guest rather than here.

The ISO-writing tests skip cleanly when ``pycdlib`` is absent, because
``run-python-tests.bat`` is the one gate that must work on a bare checkout.
"""

import hashlib
import importlib.util
import os
import shutil
import tempfile
import time
import unittest

_TOOL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "tools", "deliver-via-cd.py")
_spec = importlib.util.spec_from_file_location("deliver_via_cd", _TOOL)
deliver = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(deliver)


def _pycdlib_available():
    try:
        deliver.load_pycdlib()
    except RuntimeError:
        return False
    return True


HAVE_PYCDLIB = _pycdlib_available()


class IsoNamesTests(unittest.TestCase):
    """The 8.3 mangling, and the Joliet name that has to survive it."""

    def test_joliet_keeps_the_real_name(self):
        _iso, joliet = deliver.iso_names("NV3050Probe.kext.tar")
        self.assertEqual(joliet, "/NV3050Probe.kext.tar")

    def test_iso9660_mangles_to_eight_three(self):
        iso, _joliet = deliver.iso_names("NV3050Probe.kext.tar")
        self.assertEqual(iso, "/NV3050PR.TAR;1")

    def test_host_directory_prefix_is_stripped(self):
        _iso, joliet = deliver.iso_names(os.path.join("C:", "tmp", "notes.txt"))
        self.assertEqual(joliet, "/notes.txt")

    def test_illegal_characters_become_underscores(self):
        iso, _joliet = deliver.iso_names("my-file.txt")
        self.assertEqual(iso, "/MY_FILE.TXT;1")

    def test_extensionless_name_keeps_version_suffix(self):
        iso, _joliet = deliver.iso_names("README")
        self.assertEqual(iso, "/README.;1")

    def test_every_iso_name_carries_a_version_suffix(self):
        for name in ("notes.txt", "README", "a.b.c.tar", "x"):
            iso, _joliet = deliver.iso_names(name)
            self.assertTrue(iso.endswith(";1"), iso)


class ParseVmxTests(unittest.TestCase):
    """Key extraction, enumerated rather than counted.

    An anchored regex previously found 2 of 9 ``pciSlotNumber`` keys in this
    project because its character class excluded digits. So the expectation
    here is the full key list, not a length.
    """

    EXPECTED_KEYS = [
        ".encoding",
        "sata0:0.present", "sata0:0.fileName", "sata0:0.deviceType",
        "sata0:0.startConnected",
        "sata0:1.present", "sata0:1.fileName", "sata0:1.deviceType",
        "sata0:1.startConnected",
        "sata0:2.present", "sata0:2.fileName", "sata0:2.deviceType",
        "sata0:2.startConnected",
        "uuid.bios", "uuid.location",
    ]

    def _keys(self, text):
        return [k for _raw, k, _v in deliver.parse_vmx(text) if k is not None]

    def test_all_keys_found(self):
        self.assertEqual(self._keys(deliver.SAMPLE_VMX), self.EXPECTED_KEYS)

    def test_digit_bearing_keys_are_not_dropped(self):
        keys = self._keys(deliver.SAMPLE_VMX)
        self.assertEqual(len([k for k in keys if k.startswith("sata0:")]), 12)

    def test_comment_mentioning_a_key_is_not_a_key(self):
        # SAMPLE_VMX's comment block names deviceType = "cdrom-image" verbatim.
        for _raw, key, _value in deliver.parse_vmx(deliver.SAMPLE_VMX):
            if key is not None:
                self.assertFalse(key.startswith("#"), key)

    def test_every_line_round_trips(self):
        parsed = deliver.parse_vmx(deliver.SAMPLE_VMX)
        raws = [raw for raw, _k, _v in parsed]
        self.assertEqual(raws, deliver.SAMPLE_VMX.splitlines())

    def test_values_are_unquoted(self):
        values = dict((k, v) for _r, k, v in deliver.parse_vmx(deliver.SAMPLE_VMX)
                      if k is not None)
        self.assertEqual(values["sata0:1.fileName"], "Sonoma.vmdk")
        self.assertEqual(values["sata0:2.deviceType"], "cdrom-image")


class FindDeviceTests(unittest.TestCase):

    def setUp(self):
        self.parsed = deliver.parse_vmx(deliver.SAMPLE_VMX)

    def test_all_four_suffixes_located(self):
        found = deliver.find_device(self.parsed, "sata0:2")
        self.assertEqual(sorted(found),
                         ["deviceType", "fileName", "present", "startConnected"])

    def test_devices_do_not_bleed_into_each_other(self):
        two = deliver.find_device(self.parsed, "sata0:2")
        one = deliver.find_device(self.parsed, "sata0:1")
        self.assertTrue(two["fileName"][1].endswith("darwin.iso"))
        self.assertEqual(one["fileName"][1], "Sonoma.vmdk")

    def test_undeclared_device_is_empty_not_blank(self):
        # Absence must be an absent key, never an empty string -- otherwise
        # "declared empty" and "not declared" become indistinguishable.
        self.assertEqual(deliver.find_device(self.parsed, "sata0:3"), {})


class CheckDeviceUsableTests(unittest.TestCase):
    """The refusal that protects the installed system's VMDKs."""

    def setUp(self):
        self.parsed = deliver.parse_vmx(deliver.SAMPLE_VMX)

    def test_cdrom_image_passes(self):
        found = deliver.find_device(self.parsed, "sata0:2")
        self.assertTrue(deliver.check_device_usable(found, "sata0:2"))

    def test_disk_is_refused(self):
        for device in ("sata0:0", "sata0:1"):
            found = deliver.find_device(self.parsed, device)
            with self.assertRaises(RuntimeError) as ctx:
                deliver.check_device_usable(found, device)
            self.assertIn("refusing", str(ctx.exception))

    def test_undeclared_device_is_refused(self):
        found = deliver.find_device(self.parsed, "sata0:9")
        with self.assertRaises(RuntimeError) as ctx:
            deliver.check_device_usable(found, "sata0:9")
        self.assertIn("not declared", str(ctx.exception))

    def test_missing_filename_line_is_refused(self):
        text = "\r\n".join([
            'sata0:2.present = "TRUE"',
            'sata0:2.deviceType = "cdrom-image"',
        ]) + "\r\n"
        found = deliver.find_device(deliver.parse_vmx(text), "sata0:2")
        with self.assertRaises(RuntimeError) as ctx:
            deliver.check_device_usable(found, "sata0:2")
        self.assertIn("no fileName", str(ctx.exception))


class RepointVmxTests(unittest.TestCase):
    """The in-place edit, and everything it must leave alone."""

    ISO = "C:/Users/Kentl/macos-vm/sonoma/payload.iso"

    def setUp(self):
        self.new_text, self.old_name = deliver.repoint_vmx(deliver.SAMPLE_VMX,
                                                          self.ISO)

    def test_old_filename_is_reported_back(self):
        self.assertTrue(self.old_name.endswith("darwin.iso"), self.old_name)

    def test_new_filename_replaces_the_old(self):
        self.assertIn("payload.iso", self.new_text)
        self.assertNotIn("darwin.iso", self.new_text)

    def test_start_connected_false_is_forced_true(self):
        # A repointed drive that is not connected at power-on delivers nothing,
        # and that failure is indistinguishable from a guest ignoring the disc.
        self.assertIn('sata0:2.startConnected = "TRUE"', self.new_text)
        self.assertNotIn('sata0:2.startConnected = "FALSE"', self.new_text)

    def test_the_disks_are_untouched(self):
        self.assertIn('sata0:0.fileName = "..\\\\sonoma\\\\BaseSystem.vmdk"',
                      self.new_text)
        self.assertIn('sata0:1.fileName = "Sonoma.vmdk"', self.new_text)
        self.assertIn('sata0:0.deviceType = "disk"', self.new_text)
        self.assertIn('sata0:1.deviceType = "disk"', self.new_text)

    def test_comments_survive(self):
        self.assertIn("# Attempt 3 attached", self.new_text)

    def test_edit_is_in_place_not_appended(self):
        before = len(deliver.SAMPLE_VMX.rstrip("\r\n").split("\r\n"))
        after = len(self.new_text.rstrip("\r\n").split("\r\n"))
        self.assertEqual(after, before)

    def test_edit_stays_in_the_authored_region(self):
        # Authored content ends at uuid.bios; VMware's write-back begins at
        # uuid.location. An edit below that line is silently discarded.
        self.assertLess(self.new_text.index("payload.iso"),
                        self.new_text.index("uuid.bios"))

    def test_output_is_crlf_with_no_bare_lf(self):
        self.assertIn("\r\n", self.new_text)
        self.assertEqual(self.new_text.replace("\r\n", "").count("\n"), 0)

    def test_backslashes_are_escaped_for_vmx(self):
        text, _old = deliver.repoint_vmx(
            deliver.SAMPLE_VMX, r"C:\Users\Kentl\macos-vm\sonoma\payload.iso")
        self.assertIn(r'"C:\\Users\\Kentl\\macos-vm\\sonoma\\payload.iso"', text)

    def test_repointing_a_disk_raises_before_writing(self):
        with self.assertRaises(RuntimeError):
            deliver.repoint_vmx(deliver.SAMPLE_VMX, self.ISO, "sata0:1")


class VmRunningHeuristicTests(unittest.TestCase):
    """Lock artifacts are positive evidence only; their absence proves nothing."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="deliver-cd-test-")
        self.vmx = os.path.join(self.tmpdir, "Sonoma.vmx")
        with open(self.vmx, "w", newline="") as handle:
            handle.write(deliver.SAMPLE_VMX)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_bare_vmx_reports_nothing(self):
        self.assertEqual(deliver.vm_appears_running(self.vmx), [])

    def test_vmem_is_reported(self):
        with open(os.path.join(self.tmpdir, "Sonoma.vmem"), "wb") as handle:
            handle.write(b"\0")
        artifacts = deliver.vm_appears_running(self.vmx)
        self.assertTrue(any(p.endswith(".vmem") for p in artifacts), artifacts)

    def test_lock_directory_is_reported(self):
        os.mkdir(os.path.join(self.tmpdir, "Sonoma.vmx.lck"))
        artifacts = deliver.vm_appears_running(self.vmx)
        self.assertTrue(any(p.endswith(".vmx.lck") for p in artifacts), artifacts)


class UuidNamedArtifactTests(unittest.TestCase):
    """Regression, 2026-08-11: the VMX stem does not predict artifact names.

    ``vm_appears_running`` originally built candidates as ``stem + suffix``, so
    beside ``Sonoma.vmx`` it looked only for ``Sonoma.vmx.lck``,
    ``Sonoma.vmem`` and ``Sonoma.vmss``. Against the real running VM it
    returned ``[]`` -- VMware had named the memory file and its lock after the
    VM's UUID -- and the tool went on to rewrite the live ``Sonoma.vmx``.

    These tests are the negative control for that specific shape: a directory
    where a VM is plainly running and *no* artifact shares the VMX stem. A
    stem-relative implementation fails every assertion below.
    """

    UUID_STEM = "564d3ffb-d11c-edd5-265a-225898e79f2b"

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="deliver-cd-uuid-")
        self.vmx = os.path.join(self.tmpdir, "Sonoma.vmx")
        with open(self.vmx, "w", newline="") as handle:
            handle.write(deliver.SAMPLE_VMX)
        # Exactly the four artifacts measured in C:/Users/Kentl/macos-vm/sonoma.
        with open(os.path.join(self.tmpdir, self.UUID_STEM + ".vmem"), "wb") as h:
            h.write(b"\0")
        os.mkdir(os.path.join(self.tmpdir, self.UUID_STEM + ".vmem.lck"))
        os.mkdir(os.path.join(self.tmpdir, "Sonoma.vmdk.lck"))
        os.mkdir(os.path.join(self.tmpdir, "BaseSystem.vmdk.lck"))

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_no_artifact_shares_the_vmx_stem(self):
        """Guards the premise of this whole class, not the code under test."""
        for suffix in deliver.VMX_LOCK_SUFFIXES:
            candidate = os.path.join(self.tmpdir, "Sonoma" + suffix)
            self.assertFalse(os.path.exists(candidate), candidate)

    def test_uuid_named_vmem_is_reported(self):
        artifacts = deliver.vm_appears_running(self.vmx)
        self.assertTrue(
            any(p.endswith(self.UUID_STEM + ".vmem") for p in artifacts),
            artifacts)

    def test_uuid_named_lock_directory_is_reported(self):
        artifacts = deliver.vm_appears_running(self.vmx)
        self.assertTrue(
            any(p.endswith(self.UUID_STEM + ".vmem.lck") for p in artifacts),
            artifacts)

    def test_vmdk_locks_are_reported(self):
        artifacts = deliver.vm_appears_running(self.vmx)
        names = [os.path.basename(p) for p in artifacts]
        self.assertIn("Sonoma.vmdk.lck", names)
        self.assertIn("BaseSystem.vmdk.lck", names)

    def test_all_four_measured_artifacts_are_found(self):
        artifacts = deliver.vm_appears_running(self.vmx)
        self.assertEqual(len(artifacts), 4, artifacts)

    def test_suffix_match_is_case_insensitive(self):
        upper = os.path.join(self.tmpdir, "CRASHED.VMEM")
        with open(upper, "wb") as handle:
            handle.write(b"\0")
        names = [os.path.basename(p) for p in deliver.vm_appears_running(self.vmx)]
        self.assertIn("CRASHED.VMEM", names)


class VerifyRepointTests(unittest.TestCase):
    """Regression, 2026-08-11: the success message printed without re-reading.

    A run left ``Sonoma.vmx.bak`` on disk at 22:14:28 while ``Sonoma.vmx`` kept
    its 2026-08-09 mtime and its old ``darwin.iso`` fileName. The backup is
    copied *before* the rewrite, so its presence proves only that the tool got
    as far as copying -- nothing read the VMX back, and ``main()`` reported
    success unconditionally.

    ``verify_repoint`` is the read-back. The load-bearing property is that it
    *separates*: an un-rewritten VMX must report False. A stub returning True
    passes any test that only checks the happy path, so every check below has
    its negative twin.
    """

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="deliver-cd-verify-")
        self.iso = os.path.join(self.tmpdir, "payload.iso")
        with open(self.iso, "wb") as handle:
            handle.write(b"\0")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _write(self, name, text):
        path = os.path.join(self.tmpdir, name)
        with open(path, "w", encoding="windows-1252", newline="") as handle:
            handle.write(text)
        return path

    def _rewritten(self):
        text, _old = deliver.repoint_vmx(deliver.SAMPLE_VMX,
                                         os.path.abspath(self.iso))
        return self._write("Rewritten.vmx", text)

    def test_untouched_vmx_reports_false(self):
        ok, _actual = deliver.verify_repoint(
            self._write("Untouched.vmx", deliver.SAMPLE_VMX), self.iso)
        self.assertIs(ok, False)

    def test_untouched_vmx_reports_what_it_actually_found(self):
        # "It did not land" is not actionable on its own; the operator needs
        # the filename that is really in the file to know what happened.
        _ok, actual = deliver.verify_repoint(
            self._write("Untouched.vmx", deliver.SAMPLE_VMX), self.iso)
        self.assertIsNotNone(actual)
        self.assertTrue(actual.endswith("darwin.iso"), actual)

    def test_rewritten_vmx_reports_true(self):
        ok, _actual = deliver.verify_repoint(self._rewritten(), self.iso)
        self.assertIs(ok, True)

    def test_doubled_backslashes_are_normalised_before_comparing(self):
        # repoint_vmx escapes \ as \\ for the VMX parser. Comparing the raw
        # value against a host path would fail on every Windows path.
        _ok, actual = deliver.verify_repoint(self._rewritten(), self.iso)
        self.assertIsNotNone(actual)
        self.assertNotIn("\\\\", actual)

    def test_equivalent_path_spelled_differently_still_verifies(self):
        # main() passes os.path.abspath() into repoint_vmx, so both sides have
        # to be normalised or a correct edit reports as a failed one.
        #
        # NOT os.path.relpath() -- tempfile.mkdtemp() lands on C: while this
        # project lives on D:, and relpath raises ValueError across drives.
        odd = (os.path.dirname(self.iso).replace(os.sep, "/")
               + "/./" + os.path.basename(self.iso))
        ok, _actual = deliver.verify_repoint(self._rewritten(), odd)
        self.assertIs(ok, True)

    def test_a_different_iso_does_not_verify(self):
        # The separating case for path normalisation: normalising must not
        # collapse two genuinely different files into a match.
        other = os.path.join(self.tmpdir, "other.iso")
        with open(other, "wb") as handle:
            handle.write(b"\0")
        ok, actual = deliver.verify_repoint(self._rewritten(), other)
        self.assertIs(ok, False)
        self.assertTrue(actual.endswith("payload.iso"), actual)

    def test_device_with_no_filename_reports_false_not_true(self):
        headless = self._write("Headless.vmx",
                               'sata0:2.present = "TRUE"\r\n'
                               'sata0:2.deviceType = "cdrom-image"\r\n')
        ok, actual = deliver.verify_repoint(headless, self.iso)
        self.assertIs(ok, False)
        # None, not "" -- otherwise "declared empty" and "not declared at all"
        # become indistinguishable in the failure message.
        self.assertIsNone(actual)

    def test_wrong_device_is_checked_not_the_default(self):
        # Verifying sata0:2 while the edit went to another device would report
        # a landed edit as failed, and vice versa.
        ok, actual = deliver.verify_repoint(self._rewritten(), self.iso,
                                            device="sata0:1")
        self.assertIs(ok, False)
        self.assertEqual(actual, "Sonoma.vmdk")


class PlanEntriesTests(unittest.TestCase):

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="deliver-cd-test-")
        self.one = os.path.join(self.tmpdir, "notes.txt")
        with open(self.one, "wb") as handle:
            handle.write(b"hello from the host\n")
        self.two = os.path.join(self.tmpdir, "NV3050Probe.kext.tar")
        with open(self.two, "wb") as handle:
            handle.write(b"x" * 4096)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_sizes_come_from_the_filesystem(self):
        entries = deliver.plan_entries([self.one, self.two])
        self.assertEqual(sorted(e[3] for e in entries), [20, 4096])

    def test_missing_file_is_refused(self):
        with self.assertRaises(RuntimeError) as ctx:
            deliver.plan_entries([os.path.join(self.tmpdir, "absent.txt")])
        self.assertIn("no such file", str(ctx.exception))

    def test_directory_is_refused(self):
        with self.assertRaises(RuntimeError) as ctx:
            deliver.plan_entries([self.tmpdir])
        self.assertIn("directories are not supported", str(ctx.exception))

    def test_empty_list_is_refused(self):
        with self.assertRaises(RuntimeError) as ctx:
            deliver.plan_entries([])
        self.assertIn("nothing to deliver", str(ctx.exception))

    def test_eight_three_collision_names_both_offenders(self):
        a = os.path.join(self.tmpdir, "NV3050Probe.hpp")
        b = os.path.join(self.tmpdir, "NV3050ProbeShim.hpp")
        for path in (a, b):
            with open(path, "wb") as handle:
                handle.write(b"//\n")
        with self.assertRaises(RuntimeError) as ctx:
            deliver.plan_entries([a, b])
        message = str(ctx.exception)
        self.assertIn("collision", message)
        self.assertIn("NV3050Probe.hpp", message)
        self.assertIn("NV3050ProbeShim.hpp", message)


@unittest.skipUnless(HAVE_PYCDLIB, "pycdlib is not installed")
class BuildAndReadBackTests(unittest.TestCase):
    """The image has to be readable, and readable under its *long* name."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="deliver-cd-test-")
        self.one = os.path.join(self.tmpdir, "notes.txt")
        with open(self.one, "wb") as handle:
            handle.write(b"hello from the host\n")
        self.two = os.path.join(self.tmpdir, "NV3050Probe.kext.tar")
        with open(self.two, "wb") as handle:
            handle.write(b"x" * 4096)
        self.iso = os.path.join(self.tmpdir, "payload.iso")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_guest_sees_long_names_not_mangled_ones(self):
        deliver.build_iso(deliver.plan_entries([self.one, self.two]), self.iso)
        _ident, back = deliver.read_back(self.iso)
        self.assertIn("NV3050Probe.kext.tar", back)
        self.assertIn("notes.txt", back)
        self.assertNotIn("NV3050PR.TAR", back)

    def test_sizes_survive_the_round_trip(self):
        deliver.build_iso(deliver.plan_entries([self.one, self.two]), self.iso)
        _ident, back = deliver.read_back(self.iso)
        self.assertEqual(back["notes.txt"], 20)
        self.assertEqual(back["NV3050Probe.kext.tar"], 4096)

    def test_volume_identifier_round_trips(self):
        deliver.build_iso(deliver.plan_entries([self.one]), self.iso,
                          vol_ident="NVPAYLOAD")
        ident, _back = deliver.read_back(self.iso)
        self.assertEqual(ident, "NVPAYLOAD")

    def test_overlong_volume_identifier_is_refused(self):
        # 33 chars silently truncates on write, which would make --verify
        # disagree with --add for reasons that look like a pycdlib bug.
        with self.assertRaises(RuntimeError) as ctx:
            deliver.build_iso(deliver.plan_entries([self.one]), self.iso,
                              vol_ident="A" * 33)
        self.assertIn("max 32", str(ctx.exception))


class PinnedClockScopeTests(unittest.TestCase):
    """pinned_iso_clock() patches the `time` module process-wide, so its scope
    is the whole safety story.

    No pycdlib needed for these -- they test the context manager itself.
    """

    def test_both_patched_names_are_restored(self):
        before_time, before_localtime = time.time, time.localtime
        with deliver.pinned_iso_clock() as epoch:
            self.assertEqual(time.time(), float(epoch))
            # dates.py converts through localtime and derives the stored GMT
            # offset from it, so pinning the epoch alone would leave the image
            # TZ-dependent. gmtime fixes that offset at 0.
            self.assertIs(time.localtime, time.gmtime)
        self.assertIs(time.time, before_time)
        self.assertIs(time.localtime, before_localtime)

    def test_restored_even_when_the_block_raises(self):
        """A leaked patch would make every later test in the process nondeterministic."""
        before_time, before_localtime = time.time, time.localtime
        with self.assertRaises(RuntimeError):
            with deliver.pinned_iso_clock():
                raise RuntimeError("boom")
        self.assertIs(time.time, before_time)
        self.assertIs(time.localtime, before_localtime)

    def test_the_epoch_is_frozen(self):
        """Self-invalidating on purpose.

        Changing PINNED_ISO_EPOCH changes every payload image md5, and those md5s
        are quoted in OPERATOR-ACTION-step7-build.md and asserted by
        tests/test_operator_step7_doc.py. This test reddens so the card gets
        updated in the same change rather than a rebuild later.
        """
        self.assertEqual(deliver.PINNED_ISO_EPOCH, 1786838400)
        self.assertEqual(time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                      time.gmtime(deliver.PINNED_ISO_EPOCH)),
                         "2026-08-16T00:00:00Z")


@unittest.skipUnless(HAVE_PYCDLIB, "pycdlib is not installed")
class ReproducibleImageTests(unittest.TestCase):
    """The image must hash the same twice, and the PIN must be the reason.

    MEASURED 2026-08-16 before the pin existed: two rebuilds of
    build-payload/step7-payload.iso from byte-identical inputs were both 303104
    bytes and differed in 34 bytes across 28 runs -- 0x24 (36) vs 0x36 (54) in
    the seconds field of binary directory records, plus the ASCII '36'/'54' at
    offset 12 of the PVD date strings. The tar inside was identical both times.
    That made the md5 in the operator card a timestamp rather than a fingerprint,
    and no amount of editing the card could have kept it current.
    """

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="deliver-cd-pin-")
        self.addCleanup(shutil.rmtree, self.tmpdir, True)
        self.payload = os.path.join(self.tmpdir, "notes.txt")
        with open(self.payload, "wb") as handle:
            handle.write(b"hello from the host\n")

    def build(self, name):
        out = os.path.join(self.tmpdir, name)
        deliver.build_iso(deliver.plan_entries([self.payload]), out)
        with open(out, "rb") as handle:
            return handle.read()

    def test_two_builds_from_the_same_input_are_byte_identical(self):
        first, second = self.build("a.iso"), self.build("b.iso")
        self.assertEqual(len(first), len(second))
        self.assertEqual(hashlib.md5(first).hexdigest(),
                         hashlib.md5(second).hexdigest(),
                         "two builds from identical inputs produced different "
                         "bytes, so the image md5 is not a fingerprint")

    def test_a_different_epoch_changes_the_image(self):
        """Non-vacuity. Without this, the test above could be measuring a constant.

        Rather than mutating the source, this swaps in a pin set one day later
        and requires the bytes to MOVE. That proves two things at once: the image
        really is clock-sensitive, and build_iso() really routes through
        pinned_iso_clock() rather than merely defining it.
        """
        baseline = self.build("base.iso")
        real = deliver.pinned_iso_clock
        try:
            deliver.pinned_iso_clock = (
                lambda epoch=None: real(deliver.PINNED_ISO_EPOCH + 86400))
            shifted = self.build("shifted.iso")
        finally:
            deliver.pinned_iso_clock = real
        self.assertEqual(len(baseline), len(shifted),
                         "a date-only change altered the image SIZE, so this "
                         "test is no longer isolating the timestamps")
        self.assertNotEqual(baseline, shifted,
                           "shifting the pinned epoch by a day did not change a "
                           "single byte, so build_iso() is not using the pin and "
                           "the reproducibility test proves nothing")


class SelfTestTests(unittest.TestCase):
    """The tool's own --self-test must agree with this module."""

    @unittest.skipUnless(HAVE_PYCDLIB, "pycdlib is not installed")
    def test_self_test_passes(self):
        self.assertEqual(deliver.self_test(), 0)


if __name__ == "__main__":
    unittest.main()
