#!/usr/bin/env python3
"""Check TCAPI's headers with include-what-you-use (IWYU).

Usage: scripts/iwyu-check.py

Runs IWYU on every file git tracks under include/tcapi/, each as its own main
file, and exits nonzero when IWYU asks any of them to add or remove an include,
or cannot parse one. pre-commit runs it before each commit and each merge
commit.

The compile options come from a build the script configures itself, in
build-iwyu with the brew-tidy preset, on every run; no other build is touched.
It removes the database and the generated headers there first, so that a
configure that does not write them leaves none behind to be read. All TCAPI
sources compile with one set of options, which the headers are analyzed with.

IWYU reads the working tree, which pre-commit has reduced to what is staged,
except for files git does not track and for submodules. So the script first
stops when a file under include/ is not tracked, editor and Finder files (*~,
*.swp, *.swo, #*#, .#*, .DS_Store) aside, or when external/Cytnx differs from
the commit the index records.

IWYU is the program $IWYU names when that is set and not empty, and
include-what-you-use otherwise; a name without a directory is looked up on
PATH, /opt/homebrew/bin and /usr/local/bin. The headers were made clean with
IWYU 0.27 on macOS; another IWYU version or another standard library may report
headers nobody changed.
"""

import fnmatch
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build-iwyu"
PRESET = "brew-tidy"
CHECKED_VERSION = "0.27"
# A GUI git client's PATH lacks Homebrew, which the macOS toolchain file runs.
EXTRA_PATH = ["/opt/homebrew/bin", "/usr/local/bin"]
EDITOR_AND_FINDER_FILES = ["*~", "*.swp", "*.swo", "#*#", ".#*", ".DS_Store"]


def die(message, *details):
    print(f"error: {message}", file=sys.stderr)
    for line in details:
        print(f"  {line}", file=sys.stderr)
    sys.exit(1)


def run(args, cwd=ROOT, env=None):
    return subprocess.run(
        args,
        check=False,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def git(*args, cwd=ROOT):
    env = None
    if cwd != ROOT:
        # git exports GIT_INDEX_FILE and others to its hooks; they name this
        # repository's files, so git in a submodule must not inherit them.
        local = set(run(["git", "rev-parse", "--local-env-vars"]).stdout.split())
        env = {k: v for k, v in os.environ.items() if k not in local}
    result = run(["git", *args], cwd=cwd, env=env)
    if result.returncode != 0:
        die(f"git {' '.join(args)} failed:", *result.stdout.splitlines())
    return result.stdout


def git_paths(command, *args, cwd=ROOT):
    return [p for p in git(command, "-z", *args, cwd=cwd).split("\0") if p]


def check_tree():
    # The working tree against the index: a staged change of the pin passes,
    # while a checkout at another commit, an edited file and an untracked one
    # all differ.
    cytnx = ["--ignore-submodules=none", "--", "external/Cytnx"]
    if run(["git", "diff", "--quiet", *cytnx]).returncode != 0:
        die(
            "external/Cytnx differs from the commit the index records:",
            *git("diff", "--submodule=short", *cytnx).splitlines(),
        )
    untracked = [
        p
        for p in git_paths("ls-files", "--others", "--", "include")
        if not any(
            fnmatch.fnmatchcase(os.path.basename(p), pat)
            for pat in EDITOR_AND_FINDER_FILES
        )
    ]
    if untracked:
        die(
            "files under include/ are not tracked, and IWYU would read them:",
            *untracked,
            "add them or move them out of the tree first",
        )


def search_path():
    return os.pathsep.join([os.environ.get("PATH", ""), *EXTRA_PATH])


def searched():
    return f"PATH, {', '.join(EXTRA_PATH[:-1])} or {EXTRA_PATH[-1]}"


def find_executable(name, *message):
    """The absolute path of the executable name, or a stop with message. A name
    without a directory is looked up on search_path(); one with a directory is
    taken as it is. IWYU runs in the build directory, where a relative path
    means something else."""
    found = shutil.which(name, path=search_path())
    if found is None:
        die(*message)
    return os.path.abspath(found)


def configure():
    (BUILD / "compile_commands.json").unlink(missing_ok=True)
    shutil.rmtree(BUILD / "include", ignore_errors=True)
    cmake = find_executable("cmake", f"cmake not found on {searched()}")
    env = dict(os.environ, PATH=search_path())
    result = run([cmake, "--preset", PRESET, "-B", str(BUILD)], env=env)
    if result.returncode != 0:
        die(f"cmake --preset {PRESET} -B {BUILD} failed:", *result.stdout.splitlines())
    database = BUILD / "compile_commands.json"
    if not database.is_file():
        die(f"{database} was not written; the configure exported no compile commands")
    return database


def dropped(arg, directory, source):
    """Whether a compile option is left out: the source itself, -c, and the
    warning options, which -w replaces (-Wp, -Wl and -Wa pass options on)."""
    if arg == "-c" or os.path.realpath(os.path.join(directory, arg)) == source:
        return True
    return arg.startswith("-W") and not arg.startswith(("-Wp,", "-Wl,", "-Wa,"))


def compile_options(database):
    """The directory the TCAPI sources compile in, and the options they all compile with."""
    source_dir = os.path.realpath(ROOT / "source") + os.sep
    options = {}
    for entry in json.loads(database.read_text()):
        directory = entry["directory"]
        file = os.path.realpath(os.path.join(directory, entry["file"]))
        if not file.startswith(source_dir):
            continue
        args = (
            entry["arguments"]
            if "arguments" in entry
            else shlex.split(entry["command"])
        )
        kept = []
        rest = iter(args[1:])
        for arg in rest:
            if arg == "-o":
                next(rest, None)
            elif not dropped(arg, directory, file):
                kept.append(arg)
        options[os.path.relpath(file, ROOT)] = (directory, tuple(kept))
    if not options:
        die(
            f"{database} lists no source under {ROOT / 'source'}",
            f"the {PRESET} preset is expected to export the TCAPI target's compile commands",
        )
    distinct = set(options.values())
    if len(distinct) > 1:
        details = []
        for value in distinct:
            sources = sorted(s for s, v in options.items() if v == value)
            details.append(
                f"{', '.join(sources)}: {' '.join(value[1])} (in {value[0]})"
            )
        die(
            "TCAPI sources compile with different options, so no one set fits the headers:",
            *details,
        )
    return distinct.pop()


def find_iwyu():
    named = os.environ.get("IWYU")
    if named:
        return find_executable(named, f"IWYU={named} names no executable")
    return find_executable(
        "include-what-you-use",
        f"include-what-you-use not found on {searched()}",
        "install it with: brew install include-what-you-use, or set IWYU to it",
    )


def iwyu_version(iwyu):
    words = run([iwyu, "--version"]).stdout.split()
    return words[1] if len(words) > 1 else "unknown"


def write_mapping(path):
    """Map Cytnx's own headers to <cytnx.hpp>, its public header, and macOS's
    <_stdlib.h> to <cstdlib>."""
    cytnx = ROOT / "external" / "Cytnx"
    entries = [{"include": ["<_stdlib.h>", "private", "<cstdlib>", "public"]}]
    for p in git_paths("ls-files", "--", "include", cwd=cytnx):
        header = p[len("include/") :]
        if header.endswith(".hpp") and header != "cytnx.hpp":
            entries.append(
                {"include": [f'"{header}"', "private", "<cytnx.hpp>", "public"]}
            )
    path.write_text(json.dumps(entries, indent=1))


def report(output):
    """IWYU's output without the full include-list, which repeats what is kept."""
    lines = []
    skip = False
    for line in output.splitlines():
        if line.startswith("The full include-list for "):
            skip = True
        elif line == "---":
            skip = False
        elif not skip:
            lines.append(line)
    return "\n".join(lines).strip()


def main():
    if len(sys.argv) > 1:
        print(__doc__.strip())
        return 0 if sys.argv[1] in ("-h", "--help") else 1
    check_tree()
    iwyu = find_iwyu()
    directory, options = compile_options(configure())
    headers = git_paths("ls-files", "--", "include/tcapi")
    with tempfile.TemporaryDirectory() as tmp:
        mapping = Path(tmp) / "mapping.imp"
        write_mapping(mapping)
        flags = [
            "-Xiwyu",
            f"--mapping_file={mapping}",
            "-Xiwyu",
            "--no_fwd_decls",
            "-Xiwyu",
            "--error",
        ]

        def check(header):
            # -x c++-header, not c++: under -x c++, clang ignores #pragma once
            # in the main file, so an include cycle would parse it twice.
            cmd = [
                iwyu,
                *options,
                "-w",
                "-x",
                "c++-header",
                "-fsyntax-only",
                *flags,
                str(ROOT / header),
            ]
            return header, run(cmd, cwd=directory)

        with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
            results = list(pool.map(check, headers))

    failed = [(h, r) for h, r in results if r.returncode != 0]
    for header, result in failed:
        print(f"== {header}\n{report(result.stdout)}\n")
    if not failed:
        print(f"include-what-you-use: {len(headers)} headers checked")
        return 0
    version = iwyu_version(iwyu)
    print(
        f"include-what-you-use {version}: {len(failed)} of {len(headers)} headers failed"
    )
    if version != CHECKED_VERSION:
        print(
            f"the headers were checked with include-what-you-use {CHECKED_VERSION}, not {version}"
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
