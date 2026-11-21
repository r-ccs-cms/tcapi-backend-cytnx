# Contributing

## clang-tidy

clang-tidy checks the TCAPI library's own sources (`source/` and the headers under `include/tcapi/`) with the checks in `.clang-tidy`. Cytnx, the tests and the examples are not checked.

The repository records every warning clang-tidy reports in `.clang-tidy-baseline`. The rule is that a pull request's diff shows every change in those warnings and its description explains it: a pull request may add a warning only when it says why.

### Setup

On macOS with Homebrew:

```bash
brew install llvm
cmake --preset brew-tidy
pre-commit install
```

`cmake --preset brew-tidy` configures `build-tidy`, which runs Homebrew LLVM's clang-tidy on the library's sources as they compile and exports the `compile_commands.json` the baseline check reads. `pre-commit install` needs pre-commit 3.2.0 or later, and installs two hooks: clang-format before each commit, and the baseline check before each push.

`pre-commit install` refuses to install hooks when your git configuration sets `core.hooksPath`. Override the setting for that one command:

```bash
GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0= pre-commit install
```

The hooks are then in `.git/hooks`, and git still runs the hooks in the `core.hooksPath` directory. They run only if that directory's hooks run the ones in `.git/hooks`. If yours do not, run `scripts/tidy-count.sh --check` yourself before pushing.

### Checking and updating the baseline

```bash
scripts/tidy-count.sh            # count the warnings per check and per file
scripts/tidy-count.sh --check    # compare them with .clang-tidy-baseline
scripts/tidy-count.sh --update   # rewrite .clang-tidy-baseline
```

`--check` fails on any difference and prints each changed entry with the change in its count. An entry names a warning by its file, check, message and the text of the line it is on, without the line number, so moving code does not change it. The baseline also records each NOLINT directive with the line it covers, or a checksum of a `NOLINTBEGIN` block's lines, and how many diagnostics NOLINT directives suppress in each source's translation unit, headers included, so a warning hidden by an existing directive shows as well.

When your change fixes or adds warnings, run `--update` and commit `.clang-tidy-baseline` with the change. In the pull request description, explain every entry whose count rises.

The pre-push hook analyzes your working tree, so it fails unless the commit you push is `HEAD`, and it fails on a staged change, an untracked file, a modified submodule, or an ignored file under `source/` or `include/`, even when `status.showUntrackedFiles` or a submodule setting hides it from `git status`; untracked or ignored editor backup and swap files do not count. `--update` likewise refuses while a file under `source/` or `include/`, editor files aside, is not tracked. pre-commit sets unstaged changes to tracked files aside first, so those do not fail it. It does not see a change git is told to ignore, such as one under `git update-index --skip-worktree`. It sees one branch per push. Push one branch at a time from a clean checkout. To push without the check, run `SKIP=clang-tidy-baseline git push`.

### Warnings that are not fixed

Fix a warning where you can. A warning that cannot be fixed is handled in one of three ways.

**A warning that cannot be avoided at one site**, where disabling its check for the whole repository would be unreasonable, gets a directive on or above that site naming the check and saying why. These are the only accepted forms. A line of any file under `source/` or `include/`, whatever its extension, containing the text `NOLINT` in any other form fails the check (editor backup and swap files such as `*~` and `*.swp` aside), because clang-tidy honors that text anywhere on a line and in many spellings, some of which suppress every check:

```cpp
code; // NOLINT(<checks>): <reason>
// NOLINTNEXTLINE(<checks>): <reason>
// NOLINTBEGIN(<checks>): <reason>
// NOLINTEND(<checks>)
```

`NOLINTNEXTLINE`, `NOLINTBEGIN` and `NOLINTEND` stand alone on their line. `<checks>` is one or more check names, made of letters, digits, `.`, `_` and `-`, separated by commas without spaces or `*`. `<reason>` starts with a letter. Each `NOLINTEND` closes the innermost open `NOLINTBEGIN`, which must name the same checks.

**A warning a dependency brings in** is handled where the dependency enters, not at each site. Change `.clang-tidy` with a comment naming the dependency and why, as `.clang-tidy` disables `readability-static-accessed-through-instance` for Cytnx's `cytnx::Type.Double` style, or set `misc-include-cleaner.IgnoreHeaders` for headers internal to the dependency; or add the dependency's include directories as `SYSTEM` in CMake. A pull request that adds or updates a dependency updates the baseline like any other.

**A warning that cannot be removed**, because the TCAPI specification dictates the code or because clang-tidy is wrong, gets a directive whose reason names the specification's function or describes the false positive. For example, the specification declares `for_each`'s functor parameter as `Func&&`, and `for_each` calls the functor once per element, so it cannot forward it:

```cpp
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): tcapi::for_each takes Func&& by specification and calls f per element
```

### The baseline's environment

The baseline belongs to the clang-tidy version and the build configuration it records: its first lines hold the version and each source's `-D`, `-U`, `-std`, `-include` and `-imacros` options. `--check` compares nothing when the version differs, or when a source present in both the baseline and your build has different options, because the warnings then differ for reasons other than the code. A source added or removed is a change in the code, and shows as changed entries. A pull request that upgrades clang-tidy or changes the configuration runs `--update` and explains the change by itself.

Without the baseline's environment, compare your change with its base commit in your own environment instead. This needs a build configured with GCC or Clang, since clang-tidy cannot read the compile options of the `intel-*` presets, and the `run-clang-tidy` of LLVM 21 or 23, the versions whose output format the script has been checked against.

```bash
git worktree add ../base <base commit>
git -C ../base submodule update --init --recursive
# Configure ../base and your tree the same way, with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON.
# Unless the builds set TCAPI_CLANG_TIDY, name the clang-tidy to use; Homebrew's
# llvm is not on PATH:
export CLANG_TIDY=/opt/homebrew/opt/llvm/bin/clang-tidy
scripts/tidy-count.sh --update --baseline /tmp/base.txt --root ../base <build directory of ../base>
scripts/tidy-count.sh --check --baseline /tmp/base.txt <build directory of your tree>
```

Explain in the pull request what this shows. Someone with the baseline's environment runs `--update` before the pull request is merged.

### Limits

The check does not see a warning that replaces a fixed one with the same file, check, message and line text, nor one suppressed diagnostic replacing another under the same directives with every count unchanged. It records only the compile options named above, not, for example, `-arch`, `-O` or `-f` options, nor the versions of dependencies found through include paths. A change in any of those can change the warnings; the check then fails, and the pull request that makes the change updates the baseline.
