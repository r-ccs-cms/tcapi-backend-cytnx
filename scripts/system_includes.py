#!/usr/bin/env python3
"""Find the system include directories a build's compiler searches, for a
clang-based tool that reads the build's compile commands.

Usage: scripts/system_includes.py --root DIR DATABASE

DATABASE is a compile_commands.json and DIR the source tree it was configured
from. Every TCAPI source in it, those under DIR/source, must compile with one
compiler, in one directory, with one set of options once the source itself,
-c, -o with its output, and the warning options (-W..., but for -Wp, -Wl and
-Wa) are left out; the script stops otherwise. It asks that
compiler for its include search list under those options, and prints, one
argument per line, the options that make a clang-based tool search the same
system directories: -nostdinc++ -nostdlibinc, then each directory as
-isystem, -idirafter or -iframework followed by the directory on the next
line. A tool built apart from the system, such as the clang-tidy wheel from
PyPI or include-what-you-use from nixpkgs, does not otherwise find the
standard library the build uses on macOS. The output leaves out the
compiler's builtin-header directory, which stays the tool's own, and every
directory the options name themselves, which a tool that runs the compile
commands reads from those options: those named with -I or -F, as `-I <dir>`
or `-I<dir>`, with -iquote, -idirafter or -iframework followed by the
directory, and with `-isystem <dir>`. The tool, given the
output after the commands' own options, then searches those directories
before the printed ones, where the compiler can search one of its own, such
as /usr/local/include, before them: the directories are the compiler's, their
order not entirely.

scripts/iwyu-check.py imports these functions, and scripts/tidy-count.sh runs
this script.
"""

import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
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


def resolve(directory, path):
    """path, read from directory as a compile command reads it, made canonical."""
    return os.path.realpath(os.path.join(directory, path))


def dropped(arg, directory, source):
    """Whether a compile option is left out: the source itself, -c, and the
    warning options, which -w replaces (-Wp, -Wl and -Wa pass options on)."""
    if arg == "-c" or resolve(directory, arg) == source:
        return True
    return arg.startswith("-W") and not arg.startswith(("-Wp,", "-Wl,", "-Wa,"))


def compile_options(database, root):
    """The compiler of the TCAPI sources under root/source in database, the
    directory they compile in, and the options they all compile with."""
    source_dir = os.path.realpath(Path(root) / "source") + os.sep
    # One (source, options) pair per entry, so that a source compiled twice
    # with different options shows as different.
    options = []
    for entry in json.loads(Path(database).read_text()):
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
        options.append((os.path.relpath(file, root), (args[0], directory, tuple(kept))))
    if not options:
        die(
            f"{database} lists no source under {Path(root) / 'source'}",
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
            "TCAPI sources compile with different options, so no one set fits them all:",
            *details,
        )
    return distinct.pop()


def named_dirs(
    options,
    directory,
    separate=INCLUDE_DIR_OPTIONS,
    joined=JOINED_INCLUDE_DIR_OPTIONS,
):
    """The directories the options themselves name for the include search:
    the argument after an option in separate, and the rest of an argument
    that starts with an option in joined, as -I/path does, an argument equal
    to an option in separate excepted. INCLUDE_DIR_OPTIONS and
    JOINED_INCLUDE_DIR_OPTIONS describe the defaults."""
    dirs = set()
    rest = iter(options)
    for arg in rest:
        if arg in separate:
            value = next(rest, "")
        else:
            value = next(
                (arg[len(f) :] for f in joined if arg.startswith(f)),
                "",
            )
        if value:
            dirs.add(resolve(directory, value))
    return dirs


def without_isystem(options):
    """The options without their -isystem directories, which system_includes
    gives again in the order the build's compiler searches them."""
    kept = []
    rest = iter(options)
    for arg in rest:
        if arg == "-isystem":
            next(rest, None)
        else:
            kept.append(arg)
    return kept


def system_includes(compiler, directory, options):
    """Options that, after without_isystem(options), make a clang-based tool
    search the system include directories the build's compiler searches for
    these options, those other than framework directories in the same order.
    The -isystem directories of the options are among them, since the compiler
    can search one of its own, such as /usr/local/include, before those. The
    builtin headers stay the tool's own; framework directories go to
    -iframework, which the tool's clang searches before its builtin headers,
    where the build's compiler searches them after the system directories."""
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
            "so the tool cannot be given the directories around it in the same order",
        )
    return flags


def main():
    args = sys.argv[1:]
    if args and args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    if len(args) != 3 or args[0] != "--root":
        die("expected --root DIR DATABASE", "run with --help for usage")
    root, database = args[1], args[2]
    if not os.path.isdir(root):
        die(f"source tree {root} not found")
    if not os.path.isfile(database):
        die(f"compilation database {database} not found")
    compiler, directory, options = compile_options(database, root)
    flags = system_includes(compiler, directory, options)
    # A tool running the compile commands keeps their `-isystem <dir>`
    # options, so those directories are not given to it a second time.
    passed = named_dirs(options, directory, ("-isystem",), ())
    out = flags[:2]
    pairs = iter(flags[2:])
    for option in pairs:
        path = next(pairs)
        if resolve(directory, path) not in passed:
            out += [option, path]
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
