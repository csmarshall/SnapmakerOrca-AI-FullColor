#!/usr/bin/env python3
"""Tests for the forklifted-Snapmaker allowance in scripts/orca_profile_tool.py (ADR 0004
amendment, #109): files byte-identical to the ones pinned in scripts/snapmaker_forklift.json
are tolerated; any other bytes, or any other vendor, are held to every rule.

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import orca_profile_tool as opt  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REAL_PROFILES = os.path.join(REPO_ROOT, "resources", "profiles")


def run_check(profiles):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        errors = opt.check_profiles(profiles, ["Snapmaker"])
    return errors, out.getvalue()


@unittest.skipUnless(os.path.isdir(os.path.join(REAL_PROFILES, "Snapmaker")), "no Snapmaker bundle")
class ForkliftTolerance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.profiles = os.path.join(cls.tmp, "profiles")
        os.mkdir(cls.profiles)
        for name in ("Snapmaker", "OrcaFilamentLibrary"):
            shutil.copytree(os.path.join(REAL_PROFILES, name), os.path.join(cls.profiles, name))
            shutil.copy(os.path.join(REAL_PROFILES, name + ".json"), cls.profiles)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def forklifted(self):
        return sorted(rel for rel in opt._forklift_manifest()
                      if os.path.isfile(os.path.join(self.profiles, rel)))

    def test_manifest_files_are_recognised(self):
        files = self.forklifted()
        self.assertTrue(files)
        for rel in files:
            self.assertTrue(opt.is_forklift(self.profiles, rel), rel)

    def test_no_error_names_a_forklifted_file(self):
        _errors, text = run_check(self.profiles)
        for rel in self.forklifted():
            self.assertNotIn(rel, text)

    def test_corrupted_copy_is_held_to_every_rule(self):
        victim = next(r for r in self.forklifted() if r.endswith("@U1 0.6 nozzle.json"))
        path = os.path.join(self.profiles, victim)
        with open(path, "rb") as f:
            original = f.read()
        try:
            self.assertTrue(opt.is_forklift(self.profiles, victim))
            baseline, _ = run_check(self.profiles)
            with open(path, "wb") as f:  # a stray edit: one more space changes the bytes
                f.write(original.replace(b'"type"', b' "type"', 1))
            self.assertFalse(opt.is_forklift(self.profiles, victim))
            errors, text = run_check(self.profiles)
        finally:
            with open(path, "wb") as f:
                f.write(original)
        self.assertGreater(errors, baseline)
        self.assertIn(victim, text)

    def test_same_bytes_under_another_path_are_not_forklifted(self):
        rel = self.forklifted()[0]
        other = os.path.join(self.profiles, "Snapmaker", "filament", "copy.json")
        shutil.copy(os.path.join(self.profiles, rel), other)
        try:
            self.assertFalse(opt.is_forklift(self.profiles, "Snapmaker/filament/copy.json"))
        finally:
            os.remove(other)


if __name__ == "__main__":
    unittest.main()
