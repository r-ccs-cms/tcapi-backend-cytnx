#!/usr/bin/env python3
"""Check TCAPI's headers with include-what-you-use (IWYU).

Usage: scripts/iwyu-check.py

Runs IWYU on every file git tracks under include/tcapi/ that the working tree
holds, each as its own main file, and exits nonzero when IWYU asks any of them
to add or remove an include, or cannot parse one. pre-commit runs it before
each commit and each merge commit.

cmake and include-what-you-use are taken from PATH.

The compile options come from a build the script configures itself, in
build-iwyu with the brew preset and an empty CMake cache, on every run; no
other build is touched. It removes the database and the generated headers
there first, so that a configure that does not write them leaves none behind
to be read. All TCAPI
sources compile with one set of options, which the headers are analyzed with.
IWYU is also given the system include directories the build's compiler reports
for those options, so that it reads the standard library the build uses rather
than one of its own.

IWYU reads the working tree, which pre-commit has reduced to what is staged,
except for files git does not track and for submodules. So the script first
stops when a file under include/ or external/Cytnx/include/ is not tracked,
editor and Finder files (*~, *.swp, *.swo, #*#, .#*, .DS_Store) aside, when
external/Cytnx is checked out at a commit other than the one the index records
or has edited files, or when a sparse checkout leaves out a header under
include/tcapi/.

The headers were made clean with IWYU 0.27 against Apple clang's libc++;
another IWYU version or another standard library may report headers nobody
changed.
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
PRESET = "brew"
CHECKED_VERSION = "0.27"
EDITOR_AND_FINDER_FILES = ["*~", "*.swp", "*.swo", "#*#", ".#*", ".DS_Store"]
# Options that name an include directory as their next argument, and the two
# whose joined form, as in -I/path, is read too, the form CMake writes them in.
# Matching only these keeps an option such as -iframeworkwithsysroot from being
# read as -iframework.
INCLUDE_DIR_OPTIONS = ("-I", "-isystem", "-iquote", "-idirafter", "-iframework", "-F")
JOINED_INCLUDE_DIR_OPTIONS = ("-I", "-F")


def die(message, *details):
    print(f"error: {message}", file=sys.stderr)
    for line in details:
        print(f"  {line}", file=sys.stderr)
    sys.exit(1)


def run(args, cwd=ROOT, env=None):
    """args run to completion, with stderr in stdout. The output is decoded as
    file names are, not as text, which would turn a carriage return in a file
    name git prints into a newline."""
    result = subprocess.run(
        args,
        check=False,
        cwd=cwd,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    result.stdout = os.fsdecode(result.stdout)
    return result


def run_checked(args, shown=None, ok=(0,), cwd=ROOT, env=None):
    """run(args), stopping with its output when it exits with a code not in ok;
    shown is the command as the message names it, args joined by default."""
    if shown is None:
        shown = " ".join(args)
    result = run(args, cwd=cwd, env=env)
    if result.returncode not in ok:
        die(f"{shown} failed:", *result.stdout.splitlines())
    return result


def env_without_git_locals():
    """The environment without the variables `git rev-parse --local-env-vars`
    lists, which belong to this repository; git sets some of them, such as
    GIT_INDEX_FILE, for its hooks. A git run for another repository, as in a
    submodule or by a CMake download, must not inherit them."""
    local = set(git("rev-parse", "--local-env-vars").split())
    return {k: v for k, v in os.environ.items() if k not in local}


def git(*args, cwd=ROOT):
    env = env_without_git_locals() if cwd != ROOT else None
    return run_checked(["git", *args], cwd=cwd, env=env).stdout


def git_paths(command, *args, cwd=ROOT):
    return [p for p in git(command, "-z", *args, cwd=cwd).split("\0") if p]


def untracked(directory, cwd=ROOT):
    """Files under directory that git does not track, ignored ones included,
    editor and Finder files aside."""
    return [
        p
        for p in git_paths("ls-files", "--others", "--", directory, cwd=cwd)
        if not any(
            fnmatch.fnmatchcase(os.path.basename(p), pat)
            for pat in EDITOR_AND_FINDER_FILES
        )
    ]


def check_tree():
    # The working tree against the index: a staged change of the pin passes,
    # while a checkout at another commit and an edited file differ. Untracked
    # files in Cytnx are checked below, under include/, which IWYU reads.
    cytnx = ["--ignore-submodules=untracked", "--", "external/Cytnx"]
    # --quiet exits with 1 for a difference; any other nonzero code is a failure.
    diff = ["diff", "--quiet", *cytnx]
    if run_checked(["git", *diff], ok=(0, 1)).returncode:
        die(
            "external/Cytnx differs from the commit the index records:",
            *git("diff", "--submodule=short", *cytnx).splitlines(),
        )
    cytnx_untracked = [
        f"external/Cytnx/{p}"
        for p in untracked("include", cwd=ROOT / "external" / "Cytnx")
    ]
    for files, where in (
        (untracked("include"), "include/"),
        (cytnx_untracked, "external/Cytnx/include/"),
    ):
        if files:
            die(
                f"files under {where} are not tracked, and IWYU would read them:",
                *files,
                "add them or move them out of the tree first",
            )
    # -t tags with S each file a sparse checkout leaves out of the working tree.
    excluded = [
        t[2:]
        for t in git_paths("ls-files", "-t", "--", "include/tcapi")
        if t.startswith("S ")
    ]
    if excluded:
        die(
            "the sparse checkout leaves out headers under include/tcapi/, which IWYU cannot read:",
            *excluded,
        )


def find_executable(name):
    """The absolute path of name on PATH. IWYU runs in the build directory,
    where a relative path means something else."""
    found = shutil.which(name)
    if found is None:
        die(f"{name} not found on PATH")
    return os.path.abspath(found)


def configure():
    (BUILD / "compile_commands.json").unlink(missing_ok=True)
    shutil.rmtree(BUILD / "include", ignore_errors=True)
    # --fresh starts from an empty cache, so that a value an earlier run left
    # there, such as the default of an option() that a commit changes, does
    # not outlive the change. It keeps the dependencies already downloaded.
    command = [
        find_executable("cmake"),
        "--fresh",
        "--preset",
        PRESET,
        "-B",
        str(BUILD),
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    run_checked(command, f"cmake {' '.join(command[1:])}", env=env_without_git_locals())
    database = BUILD / "compile_commands.json"
    if not database.is_file():
        die(f"{database} was not written; the configure exported no compile commands")
    return database


def resolve(directory, path):
    """path, read from directory as a compile command reads it, made canonical."""
    return os.path.realpath(os.path.join(directory, path))


def dropped(arg, directory, source):
    """Whether a compile option is left out: the source itself, -c, and the
    warning options, which -w replaces (-Wp, -Wl and -Wa pass options on)."""
    if arg == "-c" or resolve(directory, arg) == source:
        return True
    return arg.startswith("-W") and not arg.startswith(("-Wp,", "-Wl,", "-Wa,"))


def compile_options(database):
    """The compiler of the TCAPI sources, the directory they compile in, and the
    options they all compile with."""
    source_dir = os.path.realpath(ROOT / "source") + os.sep
    # One (source, options) pair per entry, so that a source compiled twice
    # with different options shows as different.
    options = []
    for entry in json.loads(database.read_text()):
        directory = entry["directory"]
        file = resolve(directory, entry["file"])
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
        options.append((os.path.relpath(file, ROOT), (args[0], directory, tuple(kept))))
    if not options:
        die(
            f"{database} lists no source under {ROOT / 'source'}",
            "the configure is expected to export the TCAPI target's compile commands",
        )
    distinct = {value for _, value in options}
    if len(distinct) > 1:
        details = []
        for value in distinct:
            sources = sorted({s for s, v in options if v == value})
            details.append(
                f"{', '.join(sources)}: {value[0]} {' '.join(value[2])} (in {value[1]})"
            )
        die(
            "TCAPI sources compile with different options, so no one set fits the headers:",
            *details,
        )
    compiler, directory, kept = distinct.pop()
    if not any(arg.startswith("-std=") for arg in kept):
        die(
            f"the TCAPI sources in {database} compile with no -std option, so IWYU would",
            f"parse the headers with its own default standard rather than {compiler}'s;",
            "CMake leaves it out when the compiler's default meets the requirement, as",
            "GCC 16's does: run again with CXX unset, so that the configure takes the",
            "preset's default compiler",
        )
    return compiler, directory, kept


def named_dirs(options, directory):
    """The directories the options themselves name for the include search, in
    the forms INCLUDE_DIR_OPTIONS and JOINED_INCLUDE_DIR_OPTIONS describe."""
    dirs = set()
    rest = iter(options)
    for arg in rest:
        if arg in INCLUDE_DIR_OPTIONS:
            value = next(rest, "")
        else:
            value = next(
                (
                    arg[len(f) :]
                    for f in JOINED_INCLUDE_DIR_OPTIONS
                    if arg.startswith(f)
                ),
                "",
            )
        if value:
            dirs.add(resolve(directory, value))
    return dirs


def without_isystem(options):
    """The options without their -isystem directories, which system_includes
    gives IWYU again in the order the build's compiler searches them."""
    kept = []
    rest = iter(options)
    for arg in rest:
        if arg == "-isystem":
            next(rest, None)
        else:
            kept.append(arg)
    return kept


def system_includes(compiler, directory, options):
    """IWYU options that, after without_isystem(options), make its clang search
    the system include directories the build's compiler searches for these
    options, those other than framework directories in the same order. The
    -isystem directories of the options are among them, since the compiler can
    search one of its own, such as /usr/local/include, before those. The
    builtin headers stay IWYU's own; framework directories go to -iframework,
    which IWYU's clang searches before its builtin headers, where the build's
    compiler searches them after the system directories."""
    builtin = run_checked(
        [compiler, "-print-file-name=include"], cwd=directory
    ).stdout.strip()
    builtin = resolve(directory, builtin) if builtin else ""
    output = run_checked(
        [compiler, *options, "-x", "c++", "-E", "-v", os.devnull], cwd=directory
    ).stdout.splitlines()
    try:
        start = output.index("#include <...> search starts here:") + 1
        end = output.index("End of search list.", start)
    except ValueError:
        die(f"{compiler} -E -v reported no include search list")
    named = named_dirs(without_isystem(options), directory)
    flags = ["-nostdinc++", "-nostdlibinc"]
    after_builtin = False
    for line in output[start:end]:
        entry = line.strip()
        framework = entry.endswith(" (framework directory)")
        path = entry[: -len(" (framework directory)")] if framework else entry
        real = resolve(directory, path)
        if real == builtin:
            after_builtin = True
        elif real not in named:
            if framework:
                flags += ["-iframework", path]
            else:
                flags += ["-idirafter" if after_builtin else "-isystem", path]
    if not after_builtin:
        die(
            f"{compiler} -E -v did not list its builtin headers' directory, {builtin or '(none)'},",
            "so IWYU cannot be given the directories around it in the same order",
        )
    return flags


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
        if sys.argv[1] in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        die(f"unexpected argument {sys.argv[1]}", "run with --help for usage")
    check_tree()
    iwyu = find_executable("include-what-you-use")
    compiler, directory, options = compile_options(configure())
    system = system_includes(compiler, directory, options)
    # Run by hand, the script checks the working tree, which lacks a header
    # deleted but not staged; under pre-commit the header is still there. A
    # symbolic link whose target is missing is kept, so that IWYU fails on it.
    headers = [
        h
        for h in git_paths("ls-files", "--", "include/tcapi")
        if os.path.lexists(ROOT / h)
    ]
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
                *without_isystem(options),
                *system,
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
        f"include-what-you-use {version}: {len(failed)} of {len(headers)} headers failed,"
        f" with the options and standard library of {compiler}"
    )
    if version != CHECKED_VERSION:
        print(
            f"the headers were checked with include-what-you-use {CHECKED_VERSION}, not {version}"
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
