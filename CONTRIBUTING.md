# Contributing

## Setup

On macOS with Homebrew:

```bash
brew install llvm include-what-you-use
cmake --preset brew-tidy
pre-commit validate-config .pre-commit-config.yaml && pre-commit install
```

`cmake --preset brew-tidy` configures `build-tidy`, which runs Homebrew LLVM's clang-tidy on the library's sources as they compile and exports the `compile_commands.json` the baseline check reads. `pre-commit install` needs pre-commit 3.6.0 or later, which itself needs Python 3.9 or later, and installs hooks that run clang-format, ShellCheck, shfmt and the [include-what-you-use](#include-what-you-use) check before each commit and each merge commit, and the baseline check before each push. Run `pre-commit install` again whenever `default_install_hook_types` in `.pre-commit-config.yaml` changes, since it installs only the hook types listed when it runs. `pre-commit validate-config` fails on an older pre-commit before anything is installed. Without it, an older `pre-commit install` would succeed but install only the commit hook, which then stops every commit, and no other hook. Upgrade pre-commit to 3.6.0 or later and run the commands again.

`pre-commit install` refuses to install hooks when your git configuration sets `core.hooksPath`. Override the setting for that one command:

```bash
pre-commit validate-config .pre-commit-config.yaml && GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0= pre-commit install
```

The hooks are then in `.git/hooks`, and git still runs the hooks in the `core.hooksPath` directory. They run only if that directory's hooks run the ones in `.git/hooks`. If yours do not, run `scripts/iwyu-check.py` yourself before committing and `scripts/tidy-count.sh --check` before pushing.

## clang-tidy

clang-tidy checks the TCAPI library's own sources (`source/` and the headers under `include/tcapi/`) with the checks in `.clang-tidy`. Cytnx, the tests and the examples are not checked.

The repository records every warning clang-tidy reports in `.clang-tidy-baseline`. The rule is that a pull request's diff shows every change in those warnings and its description explains it: a pull request may add a warning only when it says why.

### Checking and updating the baseline

```bash
scripts/tidy-count.sh            # count the warnings per check and per file
scripts/tidy-count.sh --check    # compare them with .clang-tidy-baseline
scripts/tidy-count.sh --update   # rewrite .clang-tidy-baseline
```

`--check` fails on any difference and prints each changed entry with the change in its count. An entry names a warning by its file, check, message and the text of the line it is on, without the line number, so moving code does not change it. The baseline also records each NOLINT directive with the line it covers, or a checksum of a `NOLINTBEGIN` block's lines, and how many diagnostics NOLINT directives suppress in each source's translation unit, headers included, so a warning hidden by an existing directive shows as well.

When your change fixes or adds warnings, run `--update` and commit `.clang-tidy-baseline` with the change. In the pull request description, explain every entry whose count rises.

Each run re-runs CMake on the build directory, which applies a change to `CMakeLists.txt` but keeps the build's cache and does not reapply its preset. When `CMakePresets.json` or `CMakeUserPresets.json` was written after the build's last configure, the script stops until you configure the build again, as with `cmake --preset brew-tidy`. Any configure lifts the stop, including one that does not apply the preset, such as a build that re-runs CMake, so configure with the preset after changing it. pre-commit rewrites a file when it sets an unstaged change aside, so an unstaged change to either file stops the pre-push check every time; commit or undo it, configure the build again, then push.

The pre-push hook analyzes your working tree, so it fails unless the commit you push is `HEAD`, and it fails on a staged change, an untracked file, a submodule with changes to its tracked files or commits, or an ignored file under `source/` or `include/`, even when `status.showUntrackedFiles` or a submodule setting hides it from `git status`; untracked or ignored editor and Finder files (`*~`, `*.swp`, `*.swo`, `#*#`, `.#*`, `.DS_Store`) do not count. `--update` likewise refuses while a file under `source/` or `include/`, those files aside, is not tracked. pre-commit sets unstaged changes to tracked files aside first, so those do not fail it, except one to a preset file, as above, and one to `.pre-commit-config.yaml`, which makes pre-commit refuse to run at all. It does not see a change git is told to ignore, such as one under `git update-index --skip-worktree`. It sees one branch per push. Push one branch at a time from a clean checkout. To push without the check, run `SKIP=clang-tidy-baseline git push`.

### Warnings that are not fixed

Fix a warning where you can. A warning that cannot be fixed is handled in one of three ways.

**A warning that cannot be avoided at one site**, where disabling its check for the whole repository would be unreasonable, gets a directive on or above that site naming the check and saying why. These are the only accepted forms. A line of any file under `source/` or `include/`, or of the header CMake fills in from a template `include/<path>.in` at `<build directory>/include/<path>`, the one place the script looks for it, whatever its extension, containing the text `NOLINT` in any other form fails the check (the editor and Finder files listed above aside), because clang-tidy honors that text anywhere on a line and in many spellings, some of which suppress every check:

```cpp
code; // NOLINT(<checks>): <reason>
// NOLINTNEXTLINE(<checks>): <reason>
// NOLINTBEGIN(<checks>): <reason>
// NOLINTEND(<checks>)
```

`NOLINTNEXTLINE`, `NOLINTBEGIN` and `NOLINTEND` stand alone on their line. `<checks>` is one or more check names, made of letters, digits, `.`, `_`, `-`, `+` and `#` (as in `clang-diagnostic-c++98-compat`), separated by commas without spaces or `*`. `<reason>` starts with a letter. Each `NOLINTEND` closes the innermost open `NOLINTBEGIN`, which must name the same checks.

**A warning a dependency brings in** is handled where the dependency enters, not at each site. Change `.clang-tidy` with a comment naming the dependency and why, as `.clang-tidy` disables `readability-static-accessed-through-instance` for Cytnx's `cytnx::Type.Double` style, or set `misc-include-cleaner.IgnoreHeaders` for headers internal to the dependency; or add the dependency's include directories as `SYSTEM` in CMake. A pull request that adds or updates a dependency updates the baseline like any other.

**A warning that cannot be removed**, because the TCAPI specification dictates the code or because clang-tidy is wrong, gets a directive whose reason names the specification's function or describes the false positive. For example, the specification declares `for_each`'s functor parameter as `Func&&`, and `for_each` calls the functor once per element, so it cannot forward it:

```cpp
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): tcapi::for_each takes Func&& by specification and calls f per element
```

### The baseline's environment

The baseline belongs to the clang-tidy version and the build configuration it records: its first lines hold the version and each source's `-D`, `-U`, `-std`, `-include`, `-include-pch` and `-imacros` options, as CMake's precompiled headers pass them included. `--check` compares nothing when the version differs, or when a source present in both the baseline and your build has different options, because the warnings then differ for reasons other than the code. A source added or removed is a change in the code, and shows as changed entries. A pull request that upgrades clang-tidy or changes the configuration runs `--update` and explains the change by itself.

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

The check does not see a warning that replaces a fixed one with the same file, check, message and line text, nor one suppressed diagnostic replacing another under the same directives with every count unchanged, nor, in the rare case that the block's checksum collides, an edit inside a `NOLINTBEGIN` block. It records only the compile options named above, not, for example, `-arch`, `-O` or `-f` options, nor the versions of dependencies found through include paths. A change in any of those can change the warnings; the check then fails, and the pull request that makes the change updates the baseline.

These guarantees hold for the setup this guide describes:

- Homebrew LLVM 21 or 23 under `/opt/homebrew`, the Apple silicon prefix the `brew` presets assume;
- a build configured with `brew-tidy` or the fallback above, whose TCAPI compile commands run in the build directory, name their include directories by absolute path, and use no response file;
- pushes of one branch from a clean checkout whose remote-tracking refs are current.

They also assume that source text does not imitate clang-tidy's own output. A diagnostic message whose own text spans several lines can be misread if one of those lines looks like a `file:L:C: warning:` line, a source excerpt line such as `   12 | `, or a check list such as `[module-check]`. A NOLINT line is judged on its own, so a line whose split into code and comment depends on a block comment opened on an earlier line can be misjudged. Outside these bounds the script stops where it can tell, and may otherwise miss a change.

pre-commit runs the hook only when it finds that the push adds commits, which it judges from your local remote-tracking refs. After a branch was deleted on the remote, run `git fetch --prune` before pushing it again.

## include-what-you-use

[include-what-you-use](https://github.com/include-what-you-use/include-what-you-use) (IWYU) checks that each header under `include/tcapi/` includes a header for every name it uses and none it does not use. It analyzes each header as its own main file, so every header must also compile on its own.

The pre-commit hook runs `scripts/iwyu-check.py` on each commit and merge commit it is run for, whatever files the commit changes, and takes several seconds, since it configures a build and runs IWYU on every header. Run the script yourself to check the working tree, and `SKIP=iwyu git commit` to commit without the check.

When a header fails, the script prints the includes IWYU asks to add and to remove, or the errors that stopped it. Make those changes, except where one of the pragmas below applies.

The script configures its own build directory, `build-iwyu`, with the `brew-tidy` preset on every run, and analyzes the headers with the options the TCAPI sources compile with there. Like any configure, it keeps that directory's CMake cache from one run to the next; delete `build-iwyu` to start from an empty cache. The script needs what `cmake --preset brew-tidy` needs, and IWYU: the program `$IWYU` names when that is not empty, and `include-what-you-use` otherwise, a name without a directory being looked up on `PATH`, in `/opt/homebrew/bin` and in `/usr/local/bin`. The headers were made clean with IWYU 0.27 on macOS. Another IWYU version or another standard library can ask for changes to headers nobody changed; when a header fails, the script prints the IWYU version it ran, and says so when it is not 0.27.

IWYU reads the working tree, where pre-commit has set aside unstaged changes to tracked files but not untracked files or changes inside a submodule. So the script stops before analyzing anything when a file under `include/` is not tracked by git, editor and Finder files (`*~`, `*.swp`, `*.swo`, `#*#`, `.#*`, `.DS_Store`) aside, and when `external/Cytnx` has changes or is checked out at a commit other than the one the index records.

### Pragmas

IWYU does not see every reason a header includes another. These pragmas keep what IWYU would otherwise ask to remove:

- `cytnx_tensor_traits.h` has `// IWYU pragma: always_keep`, so IWYU keeps every include of it. The templates in a header such as `read_only_getters.h` name members of `tensor_traits<TenT>` in their declarations, and the primary `tensor_traits` is never defined, so calling one with a `CytnxTensor` needs the specializations in `cytnx_tensor_traits.h`. IWYU does not see that need in the header that includes it.
- An include at the end of a header that brings in its implementation has `// IWYU pragma: keep`, as in `io_operations.h`, `miscellaneous.h` and `version.h.in`.
- The declaration of the primary `tensor_traits` template in `tensor_traits.h` has `// IWYU pragma: keep`, since IWYU takes it for a forward declaration nothing needs.
- The includes of `tcapi.h`, the header users include, are between `// IWYU pragma: begin_exports` and `// IWYU pragma: end_exports`.

A pragma for a case not listed here gets a comment saying why.
