#!/usr/bin/env python3
"""Apply the complete repository patch to the pinned JUCE checkout."""

import pathlib
import re
import subprocess
import tempfile
from typing import NoReturn

ROOT = pathlib.Path(__file__).resolve().parents[2]
JUCE = ROOT / "JUCE-master"
JUCE_COMMIT = "29396c22c93392d6738e021b83196283d6e4d850"
PATCH_FILE = ROOT / "scripts" / "github_actions" / "juce-opentune.patch"
GATE_PATH = "modules/juce_core/threads/juce_OpenTuneCallbackGate.h"

PATCHED_PATHS = {
    "modules/juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp",
    "modules/juce_audio_devices/audio_io/juce_AudioDeviceManager.h",
    "modules/juce_audio_devices/native/juce_ASIO_windows.cpp",
    "modules/juce_audio_devices/native/juce_WASAPI_windows.cpp",
    "modules/juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h",
    "modules/juce_audio_plugin_client/juce_audio_plugin_client_ARA.cpp",
    "modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp",
    "modules/juce_audio_utils/players/juce_AudioProcessorPlayer.cpp",
    "modules/juce_audio_utils/players/juce_AudioProcessorPlayer.h",
    "modules/juce_core/threads/juce_OpenTuneCallbackGate.h",
    "modules/juce_core/threads/juce_SpinLock.h",
    "modules/juce_core/threads/juce_Thread.cpp",
    "modules/juce_gui_basics/native/juce_Windowing_windows.cpp",
}


def fail(message: str) -> NoReturn:
    raise SystemExit(message)


def git(*args):
    result = subprocess.run(
        ["git", "-C", str(JUCE), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        fail(result.stderr.decode("utf-8", errors="replace").strip())
    return result.stdout


def patch_entries(patch):
    headers = list(re.finditer(rb"(?m)^diff --git a/([^\r\n]+) b/([^\r\n]+)$", patch))
    entries = []

    for index, header in enumerate(headers):
        relative = header.group(1).decode("ascii")
        other = header.group(2).decode("ascii")
        end = headers[index + 1].start() if index + 1 < len(headers) else len(patch)
        section = patch[header.start():end]

        if relative != other or relative not in PATCHED_PATHS:
            fail(f"Unexpected JUCE patch path: {relative}")

        is_new = re.search(rb"(?m)^new file mode \d+$", section) is not None
        if is_new != (relative == GATE_PATH):
            fail(f"Unexpected file operation in JUCE patch: {relative}")

        entries.append((relative, is_new))

    if {relative for relative, _ in entries} != PATCHED_PATHS or len(entries) != len(PATCHED_PATHS):
        fail("The JUCE patch does not contain the exact supported file set")

    for hook in (b"OPENTUNE_REALTIME_AUDIT_SPINLOCK();",
                 b"OPENTUNE_REALTIME_AUDIT_SPIN_WAIT();"):
        if patch.count(hook) != 1:
            fail("The JUCE patch must contain both realtime SpinLock audit hooks")

    return entries


def build_targets(entries, patch):
    with tempfile.TemporaryDirectory(prefix="opentune-juce-patch-") as directory:
        staging = pathlib.Path(directory)

        for relative, is_new in entries:
            destination = staging.joinpath(*relative.split("/"))
            destination.parent.mkdir(parents=True, exist_ok=True)

            if not is_new:
                destination.write_bytes(normalized(git("show", f"HEAD:{relative}")))

        result = subprocess.run(
            ["git", "apply", "--whitespace=nowarn", "-"],
            cwd=staging,
            input=patch,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode:
            fail(result.stderr.decode("utf-8", errors="replace").strip())

        return {
            relative: staging.joinpath(*relative.split("/")).read_bytes()
            for relative, _ in entries
        }


def normalized(data):
    return data.replace(b"\r\n", b"\n")


def merged_file(current, base, target):
    with tempfile.TemporaryDirectory(prefix="opentune-juce-merge-") as directory:
        temporary = pathlib.Path(directory)
        current_path = temporary / "current"
        base_path = temporary / "base"
        target_path = temporary / "target"
        current_path.write_bytes(current)
        base_path.write_bytes(base)
        target_path.write_bytes(target)

        result = subprocess.run(
            ["git", "merge-file", "-p", str(current_path), str(base_path), str(target_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode:
            return None

        return normalized(result.stdout)


def has_staged_changes(relative):
    result = subprocess.run(
        ["git", "-C", str(JUCE), "diff", "--cached", "--quiet", "HEAD", "--", relative],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode > 1:
        fail(result.stderr.decode("utf-8", errors="replace").strip())
    return result.returncode == 1


def prepare_changes(targets, entries):
    new_paths = {relative for relative, is_new in entries if is_new}
    changes = {}

    for relative, target_bytes in targets.items():
        target = normalized(target_bytes)
        path = JUCE.joinpath(*relative.split("/"))

        if path.is_file():
            current_bytes = path.read_bytes()
            current = normalized(current_bytes)
        elif path.exists():
            fail(f"JUCE patch target is not a file: {relative}")
        else:
            current_bytes = None
            current = None

        if current == target:
            continue

        if relative in new_paths:
            if current is not None:
                fail(f"JUCE callback gate differs from the repository patch: {relative}")
        else:
            if current is None:
                fail(f"JUCE patch target is missing: {relative}")

            base = normalized(git("show", f"HEAD:{relative}"))
            if current != base and merged_file(current, base, target) != target:
                fail(f"JUCE {relative} contains changes outside the complete repository patch")

        if has_staged_changes(relative):
            fail(f"JUCE {relative} has staged changes; unstaged target files are left untouched")

        newline = b"\r\n" if current_bytes is not None and b"\r\n" in current_bytes else b"\n"
        changes[relative] = target.replace(b"\n", newline)

    return changes


def main():
    head = git("rev-parse", "HEAD").decode("ascii").strip()
    if head != JUCE_COMMIT:
        fail(f"Expected JUCE {JUCE_COMMIT}, found {head}")

    if not PATCH_FILE.is_file():
        fail(f"JUCE patch file not found: {PATCH_FILE}")

    patch = normalized(PATCH_FILE.read_bytes())
    entries = patch_entries(patch)
    targets = build_targets(entries, patch)
    changes = prepare_changes(targets, entries)

    for relative, contents in changes.items():
        JUCE.joinpath(*relative.split("/")).write_bytes(contents)

    if changes:
        print(f"Complete JUCE repository patch applied ({len(changes)} files)")
    else:
        print("Complete JUCE repository patch is already applied")


if __name__ == "__main__":
    main()
