"""Compiles the real firmware version in, instead of a hardcoded one.

platformio.ini used to carry `-DLNURLVAULT_FW_VERSION=\"0.1.0\"` in both
environments, so `get_info` reported 0.1.0 on every release ever built --
which is worse than reporting nothing, because a bug report quoting it sends
you to the wrong commit. This is a PlatformIO pre-build extra_script (see
platformio.ini's `extra_scripts`) that works the version out instead.

Precedence, most authoritative first:

  1. $LNURLVAULT_FW_VERSION -- what CI sets from the pushed tag, so a release
     is stamped with exactly the tag it was cut from and nothing else.
  2. `git describe --tags --always --dirty` -- for a local build. Gives
     `v0.1.0` on a tag, `v0.1.0-3-gabc1234` three commits past one, and a
     `-dirty` suffix when the tree has uncommitted changes. A vault that
     discloses secrets should say out loud that it is running something not
     in the history.
  3. `0.0.0-unknown` -- no tag, no git, or git not on PATH. Deliberately not
     a plausible-looking number: an honest "I do not know" is safe, and a
     wrong version is not.

A leading `v` is stripped so the wire value is `0.1.0`, matching what
docs/PROTOCOL.md's get_info example shows, while the git tags stay `v*` as
release.yml's trigger expects.
"""

import os
import subprocess

Import("env")  # noqa: F821 -- injected by PlatformIO's SCons environment


def _from_git():
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            capture_output=True,
            text=True,
            timeout=10,
            cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip() or None


def _resolve():
    version = os.environ.get("LNURLVAULT_FW_VERSION", "").strip()
    if not version:
        version = _from_git()
    if not version:
        return "0.0.0-unknown"
    return version[1:] if version.startswith("v") else version


VERSION = _resolve()
print(f"lnurl-vault firmware version: {VERSION}")

env.Append(  # noqa: F821
    CPPDEFINES=[("LNURLVAULT_FW_VERSION", env.StringifyMacro(VERSION))]  # noqa: F821
)
