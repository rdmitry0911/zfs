#!/usr/bin/env python3
"""Exercise commit metadata validation with real, isolated Git commits."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

CHECKER = Path(__file__).resolve().parents[4] / "scripts/commitcheck.sh"


class CommitCheckTests(unittest.TestCase):
    def check_message(self, message):
        with tempfile.TemporaryDirectory(prefix="zfs-commitcheck-") as scratch:
            env = dict(os.environ, GIT_AUTHOR_NAME="Test Author",
                       GIT_AUTHOR_EMAIL="test@example.com",
                       GIT_COMMITTER_NAME="Test Author",
                       GIT_COMMITTER_EMAIL="test@example.com")
            subprocess.run(["git", "init", "--quiet", scratch], check=True,
                           env=env)
            subprocess.run(["git", "-C", scratch, "commit", "--quiet",
                            "--allow-empty", "--no-gpg-sign", "-F", "-"],
                           input=message, text=True, env=env, check=True)
            return subprocess.run(["sh", str(CHECKER)], cwd=scratch,
                                  capture_output=True, text=True).returncode

    def test_complete_coauthor_identity_is_kept_intact(self):
        trailer = ("Co-authored-by: Tigerblue77 "
                   "<37409593+tigerblue77@users.noreply.github.com>")
        self.assertGreater(len(trailer), 72)
        self.assertEqual(self.check_message(
            "Describe a change\n\n" + trailer + "\n"
            "Signed-off-by: Test Author <test@example.com>\n"), 0)

    def test_long_prose_still_fails(self):
        self.assertNotEqual(self.check_message(
            "Describe a change\n\n" + "a" * 73 + "\n"
            "Signed-off-by: Test Author <test@example.com>\n"), 0)

    def test_incomplete_coauthor_does_not_exempt_prose(self):
        self.assertNotEqual(self.check_message(
            "Describe a change\n\nCo-authored-by: " + "a" * 73 + "\n"
            "Signed-off-by: Test Author <test@example.com>\n"), 0)

    def test_missing_signoff_still_fails(self):
        self.assertNotEqual(self.check_message(
            "Describe a change\n\nCo-authored-by: A <a@example.com>\n"), 0)


if __name__ == "__main__":
    unittest.main()
