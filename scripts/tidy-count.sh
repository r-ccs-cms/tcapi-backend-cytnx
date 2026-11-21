#!/usr/bin/env bash
# Count clang-tidy warnings on TCAPI's sources in a form that is comparable
# from one run to the next, and compare them with the committed baseline.
#
# Usage: scripts/tidy-count.sh [--check | --update] [--baseline FILE]
#                              [--root DIR] [BUILD_DIR]
#
#   (no mode)  print the counts
#   --update   write the baseline FILE from the current warnings, refusing
#              while a file under DIR/source or DIR/include other than an
#              editor or Finder file (see below) is not tracked by git
#   --check    compare the current warnings with the baseline FILE; exit
#              nonzero on any difference
#
# DIR is the source tree to analyze (default: the repository holding this
# script), BUILD_DIR a CMake build directory configured from DIR (default:
# DIR/build-tidy, which `cmake --preset brew-tidy` creates), and FILE the
# baseline (default: .clang-tidy-baseline in the repository holding this
# script). The clang-tidy binary is taken from $CLANG_TIDY, else from
# TCAPI_CLANG_TIDY in BUILD_DIR's cache, else from PATH.
#
# The build log is not a usable count: an incremental build re-runs clang-tidy
# only on the sources it recompiles, and the compiler's own -W warnings are
# mixed into it. This script instead re-runs CMake on BUILD_DIR, setting
# CMAKE_EXPORT_COMPILE_COMMANDS=ON in its cache, so that its
# compile_commands.json and generated headers match the sources (that keeps
# the cache and reapplies no preset, so it first stops if DIR's
# CMakePresets.json or CMakeUserPresets.json was written after BUILD_DIR's
# last configure, until BUILD_DIR is configured again), then runs
# clang-tidy on every TCAPI source in that database and normalizes the output:
#   - a warning in a header is reported once per source that includes it, so
#     warnings are deduplicated by location, check and message;
#   - paths are made relative to DIR, and BUILD_DIR's own path is written as
#     <build>, so generated headers do not depend on its name;
#   - a warning listed under several aliases ([a,b]) counts once.
#
# The baseline holds the clang-tidy version line, the configuration (each
# source's -D, -U, -std, -include and -imacros options), and counts of these
# keys:
#   warning     file, checks, message and the text of the flagged line, so a
#               change that only moves lines changes no warning key;
#   nolint      file, the directive's line and the line it covers, or for a
#               NOLINTBEGIN block a checksum of the lines up to its NOLINTEND,
#               which changes when those lines are edited, reordered, or moved
#               into or out of the block;
#   suppressed  each source's count of diagnostics suppressed by NOLINT, which
#               changes when an existing directive starts hiding a new one.
# --check compares nothing when the version, or the options of a source
# present on both sides, differ: the counts would then differ for reasons
# other than the code.
#
# Every line of every file under DIR/source and DIR/include, editor and Finder
# files (*~, *.swp, *.swo, #*#, .#*, .DS_Store) aside, containing the text NOLINT must
# be one directive in one of these forms, because clang-tidy honors that text
# anywhere on a line, in many spellings:
#   // NOLINT(<checks>): <reason>
#   // NOLINTNEXTLINE(<checks>): <reason>
#   // NOLINTBEGIN(<checks>): <reason>
#   // NOLINTEND(<checks>)
# NOLINT follows the code it applies to; the other three stand alone on their
# line. <checks> is a comma-separated list of check names (letters, digits, `.`,
# `_`, `-`, `+` and `#`) without spaces or `*`, <reason> starts with a letter, and each
# NOLINTEND closes the innermost open NOLINTBEGIN, which must name the same
# checks.
#
# Run by pre-commit at pre-push, the script also requires that the pushed
# commit is HEAD, that `git status` lists no change pre-commit leaves in place
# when told to show every untracked file and submodule change (untracked
# files inside a submodule aside), and that no
# ignored file lies under DIR/source or DIR/include, untracked and ignored
# editor and Finder files aside. Changes git is told to
# ignore, such as with update-index --skip-worktree, are not seen.

set -euo pipefail

die() {
  printf 'error: %s\n' "$1" >&2
  shift
  (($# == 0)) || printf '%s\n' "$@" | awk '{ print "  " $0 }' >&2
  exit 1
}

# Prints the absolute path of the directory $1.
abspath() {
  (CDPATH='' cd -- "$1" && pwd)
}

# Prints the absolute path of the file $1, whose directory exists.
abspath_file() {
  local dir
  dir=$(abspath "$(dirname -- "$1")") || return
  printf '%s/%s\n' "$dir" "$(basename -- "$1")"
}

# Prints the value of the entry $1 in BUILD_DIR's CMakeCache.txt, a line of
# the form NAME:TYPE=VALUE whose VALUE may itself contain `=`.
cache_value() {
  awk -v name="$1:" 'index($0, name) == 1 { sub(/^[^=]*=/, ""); print; exit }' "$build/CMakeCache.txt"
}

usage() {
  awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
}

mode=count
baseline=
root=
build_arg=
while (($#)); do
  case $1 in
    --check | --update)
      [[ $mode == count ]] || die "--check and --update cannot be combined"
      mode=${1#--}
      ;;
    --baseline | --root)
      (($# >= 2)) && [[ -n $2 && $2 != -* ]] || die "$1 needs a path, not ${2:-nothing}"
      if [[ $1 == --baseline ]]; then baseline=$2; else root=$2; fi
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    -*) die "unknown option: $1" "run with --help for usage" ;;
    *)
      [[ -n $1 ]] || die "BUILD_DIR is empty"
      [[ -z $build_arg ]] || die "more than one BUILD_DIR: $build_arg, $1"
      build_arg=$1
      ;;
  esac
  shift
done
# The --root and BUILD_DIR given, repeated in the --update a message suggests.
rerun=
[[ -z $root ]] || rerun+=" --root $(printf %q "$root")"
[[ -z $build_arg ]] || rerun+=" $(printf %q "$build_arg")"

script_repo=$(abspath "$(dirname "$0")/..")
if [[ -n $root ]]; then
  [[ -d $root ]] || die "source tree $root not found"
  root=$(abspath "$root")
else
  root=$script_repo
fi
default_baseline=$script_repo/.clang-tidy-baseline
baseline=${baseline:-$default_baseline}
# Whether FILE is the repository's baseline: the same file, or, while it does
# not exist, the same path once made absolute.
is_default=
if [[ $baseline -ef $default_baseline ]] ||
  { [[ -d $(dirname -- "$baseline") ]] && [[ $(abspath_file "$baseline") == "$default_baseline" ]]; }; then
  is_default=1
fi
[[ ! -d $baseline ]] || die "baseline $baseline is a directory; give the path of a file"
build_arg=${build_arg:-$root/build-tidy}
[[ -d $build_arg ]] ||
  die "build directory $build_arg not found" "configure it with: cmake --preset brew-tidy"
build=$(abspath "$build_arg")
[[ -f $build/CMakeCache.txt ]] ||
  die "$build is not a CMake build directory" "configure it with: cmake --preset brew-tidy"

tidy=${CLANG_TIDY:-}
if [[ -z $tidy ]]; then
  tidy=$(cache_value TCAPI_CLANG_TIDY)
fi
tidy=${tidy:-clang-tidy}
tidy_path=$(command -v "$tidy" || true)
[[ -n $tidy_path ]] || die "clang-tidy not found: $tidy" "install it with: brew install llvm, or set CLANG_TIDY to the clang-tidy to use"
runner="$(dirname "$tidy_path")/run-clang-tidy"
[[ -x $runner ]] || runner=$(command -v run-clang-tidy || true)
[[ -n $runner ]] || die "run-clang-tidy not found" "install it with: brew install llvm"
# Both run after a cd into DIR, so a path relative to here must not reach them.
tidy_path=$(abspath_file "$tidy_path")
runner=$(abspath_file "$runner")
version_out=$("$tidy_path" --version 2>&1) || die "cannot run $tidy_path --version:" "$version_out"
version=$(printf '%s\n' "$version_out" | awk 'tolower($0) ~ /version/ { sub(/^[ \t]+/, ""); print; exit }')

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Awk helpers shared by the programs below: path normalization and trimming.
# norm() and rel() take the two prefixes they rewrite from norm_vars, set once
# the database's spelling of DIR and BUILD_DIR is known.
awk_lib='
function norm(p) {
  if (index(p, build) == 1) return "<build>/" substr(p, length(build) + 1)
  if (index(p, root) == 1) return substr(p, length(root) + 1)
  return p
}
# A key field that would be empty is written as -, so that no baseline line
# ends in a tab that an editor or a patch tool could strip.
function or_dash(s) {
  return s == "" ? "-" : s
}
# Writes every BUILD_DIR/ and DIR/ in s, wherever it occurs, as <build>/ and
# <root>/, so that a path inside an option such as -DP=DIR/inc is rewritten too.
function rel(s) {
  return replace_all(replace_all(s, build, "<build>/"), root, "<root>/")
}
function trim(s) {
  sub(/\r$/, "", s)
  gsub(/\t/, " ", s)
  sub(/^ +/, "", s)
  sub(/ +$/, "", s)
  return s
}
function replace_all(s, from, to,    out, i) {
  out = ""
  while ((i = index(s, from)) > 0) {
    out = out substr(s, 1, i - 1) to
    s = substr(s, i + length(from))
  }
  return out s
}
'

# Drops the lines of standard input naming an untracked or ignored editor or
# Finder file (*~, *.swp, *.swo, #*#, .#*, .DS_Store), the files the NOLINT scan's
# find also skips. With `status`, the lines are `git status --porcelain` lines,
# and only `?? ` and `!! ` lines can be dropped; otherwise each line is a path.
drop_editor_files() {
  awk -v status="${1:-}" '{
    p = $0
    if (status != "") {
      if (p !~ /^(\?\?|!!) /) { print; next }
      p = substr(p, 4)
    }
    # git quotes a path holding special characters as a whole.
    if (p ~ /^".*"$/) p = substr(p, 2, length(p) - 2)
    n = split(p, parts, "/")
    b = parts[n]
    if (b ~ /~$/ || b ~ /\.sw[po]$/ || b ~ /^#.*#$/ || b ~ /^\.#/ || b == ".DS_Store") next
    print
  }'
}

# Counts identical lines of standard input as `<count>\t<line>`.
count_keys() {
  LC_ALL=C sort | uniq -c | awk '{ c = $1; sub(/^ *[0-9]+ /, ""); print c "\t" $0 }'
}

# Prints the number of lines in the file $1.
count_lines() {
  wc -l <"$1" | tr -d ' '
}

# Counts identical lines of standard input, most frequent first.
rank_lines() {
  LC_ALL=C sort | uniq -c | LC_ALL=C sort -k1,1nr -k2
}

fallback_hint() {
  cat >&2 <<EOF
To compare in this environment, measure the change's base commit here too
(GCC or Clang builds only, since clang-tidy cannot read the intel-* toolchain's
flags, with the run-clang-tidy of LLVM 21 or 23, whose output this script reads):
  git worktree add <dir> <base commit>
  git -C <dir> submodule update --init --recursive
  configure <dir> and this tree the same way, with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  set CLANG_TIDY to the clang-tidy to use, unless the builds set TCAPI_CLANG_TIDY
  $0 --update --baseline <file> --root <dir> <dir's build directory>
  $0 --check --baseline <file> <this tree's build directory>
CONTRIBUTING.md describes this. To push without the check: SKIP=clang-tidy-baseline git push
EOF
}

# --- Checks that need no analysis --------------------------------------------

if [[ $mode == check ]]; then
  if [[ ! -f $baseline ]]; then
    if [[ -n $is_default ]]; then
      die "baseline $baseline not found" "write it with: $0 --update$rerun"
    fi
    die "baseline $baseline not found" \
      "write it from the base commit with: $0 --update --baseline $baseline --root <base worktree> <its build directory>"
  fi
  base_version=$(awk -F'\t' '$1 == "version" { print $2; exit }' <"$baseline")
  if [[ $base_version != "$version" ]]; then
    printf 'error: clang-tidy version differs from the baseline; nothing was compared\n' >&2
    printf '  baseline: %s\n  running:  %s\n' "$base_version" "$version" >&2
    fallback_hint
    exit 1
  fi
fi

# pre-commit sets PRE_COMMIT for every run, and PRE_COMMIT_REMOTE_NAME only
# when a push runs the hook.
if [[ ${PRE_COMMIT:-} == 1 && -n ${PRE_COMMIT_REMOTE_NAME:-} ]]; then
  [[ -n ${PRE_COMMIT_TO_REF:-} ]] ||
    die "pre-commit did not say which commit is pushed, as for a push that reaches a root commit" \
      "run $0 --check by hand, then push with SKIP=clang-tidy-baseline"
  # A pushed annotated tag arrives as the tag object; compare the commit.
  pushed=$(git -C "$root" rev-parse "$PRE_COMMIT_TO_REF^{commit}")
  [[ $pushed == "$(git -C "$root" rev-parse HEAD)" ]] ||
    die "the pushed commit $pushed is not HEAD, which is what this check analyzes" \
      "check out the branch you push, and push one branch at a time"
  # Spelled out so that status.showUntrackedFiles and the submodule settings
  # cannot hide a change, untracked files inside a submodule aside, and ignored files under source/ and include/ count
  # too, since the analysis reads every file there but editor and Finder files.
  # Each runs on its own, so that neither one's failure can pass for a clean tree.
  status_out=$(git -C "$root" status --porcelain --untracked-files=all --ignore-submodules=untracked) ||
    die "git status failed, so whether the working tree is HEAD cannot be checked"
  ignored_out=$(git -C "$root" ls-files --others --ignored --exclude-standard -- source include) ||
    die "git ls-files failed, so whether the working tree is HEAD cannot be checked"
  dirty=$({
    printf '%s\n' "$status_out"
    printf '%s\n' "$ignored_out" | awk 'length { print "!! " $0 }'
  } | drop_editor_files status | awk 'length')
  [[ -z $dirty ]] ||
    die "the working tree differs from HEAD, which is what is pushed:" "$dirty" \
      "commit or undo these changes, or move the listed files out of the tree, before pushing"
fi

# The analysis reads every file under source/ and include/ but editor and Finder files, so
# a baseline written with files git does not track holds entries nobody else
# reproduces.
if [[ $mode == update ]] && git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
  untracked=$(git -C "$root" ls-files --others -- source include | drop_editor_files)
  [[ -z $untracked ]] ||
    die "--update would record files git does not track:" "$untracked" \
      "commit them or move them out of the tree first"
fi

# --- Configure, and check the compilation database ---------------------------

home=$(cache_value CMAKE_HOME_DIRECTORY)
[[ -n $home && $home -ef $root ]] ||
  die "$build was configured from ${home:-an unknown tree}, not from $root"
# compile_commands.json and clang-tidy spell paths the way CMake was configured
# with them, which differs from DIR and BUILD_DIR when CMake reached the same
# directories by another path, such as through a symbolic link, so match
# against CMake's spelling from here on.
root=$home
cachedir=$(cache_value CMAKE_CACHEFILE_DIR)
[[ -n $cachedir && $cachedir -ef $build ]] && build=$cachedir
norm_vars=(-v "build=$build/" -v "root=$root/")
# Re-running CMake on BUILD_DIR keeps its cache and does not reapply a preset,
# so a preset file changed since the last configure would go unapplied. Every
# configure rewrites cmake.check_cache; stopping before the re-run below keeps
# this check failing until BUILD_DIR is configured again. macOS's bash 3.2
# compares the times to the second, so a write within the second of the
# configure goes unseen there. Any configure clears
# it, one that reapplies no preset included, such as a plain `cmake BUILD_DIR`
# or a build that re-runs CMake after CMakeLists.txt changed.
stamp=$build/CMakeFiles/cmake.check_cache
[[ -f $stamp ]] || stamp=$build/CMakeCache.txt
for presets in "$root/CMakePresets.json" "$root/CMakeUserPresets.json"; do
  [[ ! $presets -nt $stamp ]] ||
    die "$presets was written after $build was last configured" \
      "configure it again, with its preset if it has one, as in: cmake --preset brew-tidy" \
      "in a push with an unstaged change to it, commit or undo that change first"
done
# The CMake that configured BUILD_DIR, which need not be on PATH, as in a hook
# a GUI client runs. A preset's environment variables are not reapplied.
cmake_command=$(cache_value CMAKE_COMMAND)
[[ -x $cmake_command ]] || cmake_command=cmake
if ! "$cmake_command" "$build" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$work/cmake.log" 2>&1; then
  cat "$work/cmake.log" >&2
  die "re-running CMake on $build failed"
fi
[[ -f $build/compile_commands.json ]] || die "$build/compile_commands.json was not written"

# One `<file>\t<command>` line per entry of the database.
awk '
function value(line) {
  sub(/^[ \t]*"[a-z]+": "/, "", line)
  sub(/",?[ \t]*$/, "", line)
  gsub(/\\"/, "\"", line)
  gsub(/\\\\/, "\\", line)
  return line
}
/^[ \t]*"command": "/ { cmd = value($0) }
/^[ \t]*"file": "/ { print value($0) "\t" cmd }
' "$build/compile_commands.json" >"$work/db.tsv"

awk -F'\t' -v p="$root/source/" 'index($1, p) == 1 { print $1 }' "$work/db.tsv" |
  LC_ALL=C sort -u >"$work/db-sources"
find "$root/source" -type f -name '*.cpp' | LC_ALL=C sort >"$work/disk-sources"
missing=$(LC_ALL=C comm -23 "$work/disk-sources" "$work/db-sources")
[[ -z $missing ]] ||
  die "these sources are not in $build/compile_commands.json, so they would not be analyzed:" \
    "$missing" "a unity build or a source CMake does not compile leaves them out"
nsources=$(count_lines "$work/db-sources")
((nsources > 0)) || die "no source under $root/source is in $build/compile_commands.json"

# One `config\t<source>\t<options>` line per source.
awk -F'\t' "${norm_vars[@]}" -v p="$root/source/" "$awk_lib"'
# Splits the shell command line s into words in w, as sh would: quotes group,
# and a backslash escapes the next character outside single quotes.
function shell_words(s, w,    n, i, c, cur, q, started) {
  n = 0; cur = ""; q = ""; started = 0
  for (i = 1; i <= length(s); i++) {
    c = substr(s, i, 1)
    if (q == "") {
      if (c == " " || c == "\t") { if (started) { w[++n] = cur; cur = ""; started = 0 }; continue }
      started = 1
      if (c == "\"" || c == "'"'"'") { q = c; continue }
      if (c == "\\" && i < length(s)) { i++; c = substr(s, i, 1) }
      cur = cur c
    } else if (c == q) q = ""
    else {
      if (q == "\"" && c == "\\" && i < length(s)) { i++; c = substr(s, i, 1) }
      cur = cur c
    }
  }
  if (started) w[++n] = cur
  return n
}
# A word holding a space is written quoted, so that the words stay apart.
function word(s) { return s ~ /[ \t]/ ? "\"" s "\"" : s }
index($1, p) == 1 {
  n = shell_words($2, t)
  opts = ""
  for (i = 1; i <= n; i++) {
    if (t[i] ~ /^-[DU]./ || t[i] ~ /^-std=/ || t[i] ~ /^-include./ || t[i] ~ /^-imacros./) opts = opts " " word(rel(t[i]))
    else if ((t[i] == "-D" || t[i] == "-U" || t[i] == "-include" || t[i] == "-imacros") && i < n) { opts = opts " " t[i] " " word(rel(t[i + 1])); i++ }
  }
  sub(/^ /, "", opts)
  print "config\t" norm($1) "\t" or_dash(opts)
}' "$work/db.tsv" | LC_ALL=C sort -u >"$work/config"

if [[ $mode == check ]]; then
  awk -F'\t' '$1 == "config"' <"$baseline" | LC_ALL=C sort -u >"$work/base-config"
  mismatch=$(awk -F'\t' '
    # A source compiled by several targets has several lines; compare them as
    # one set per source. Both files are sorted, so the sets concatenate alike.
    NR == FNR { base[$2] = base[$2] "\n      " $3; next }
    { cur[$2] = cur[$2] "\n      " $3 }
    END { for (s in cur) if ((s in base) && base[s] != cur[s]) printf "  %s\n    baseline:%s\n    current:%s\n", s, base[s], cur[s] }
  ' "$work/base-config" "$work/config")
  if [[ -n $mismatch ]]; then
    printf 'error: compile options differ from the baseline; nothing was compared\n%s\n' "$mismatch" >&2
    fallback_hint
    exit 1
  fi
fi

# --- NOLINT directives ---------------------------------------------------------

# Writes `nolint\t...` keys to nolint.keys, `<check>` lines to nolint.checks and
# violations to nolint.errors.
# Every file, whatever its extension, since clang-tidy reads any file a source
# includes, except editor and Finder files, which no source includes. The
# patterns are drop_editor_files's.
find -L "$root/source" "$root/include" -type f \
  ! -name '*~' ! -name '*.swp' ! -name '*.swo' ! -name '#*#' ! -name '.#*' ! -name .DS_Store \
  -exec env LC_ALL=C awk \
  "${norm_vars[@]}" \
  -v keys="$work/nolint.keys" -v checks_out="$work/nolint.checks" -v errors="$work/nolint.errors" \
  "$awk_lib"'
BEGIN {
  for (i = 1; i < 256; i++) ord[sprintf("%c", i)] = i
  directive = "// NOLINT(NEXTLINE|BEGIN|END)?\\([A-Za-z0-9_.+#-]+(,[A-Za-z0-9_.+#-]+)*\\)"
}
function bad(msg) { print file ":" FNR ": " msg >> errors }
function end_file() {
  if (file == "") return
  if (pending != "") { print pending "\t-" >> keys; pending = "" }
  for (; depth > 0; depth--) print file ":" open_line[depth] ": NOLINTBEGIN(" open_checks[depth] ") is never closed" >> errors
}
function hash_line(s,    i) {
  for (d = 1; d <= depth; d++) {
    for (i = 1; i <= length(s); i++) h[d] = (h[d] * 31 + ord[substr(s, i, 1)]) % 2147483629
    h[d] = (h[d] * 31 + 10) % 2147483629
  }
}
FNR == 1 { end_file(); file = norm(FILENAME); depth = 0 }
{
  # A CRLF line ending would read as text after the directive.
  sub(/\r$/, "")
  if (pending != "") { print pending "\t" or_dash(trim($0)) >> keys; pending = "" }
  line = $0
  if (index(line, "NOLINT") == 0) { hash_line(line); next }
  copy = line
  if (gsub(/NOLINT/, "", copy) != 1) { bad("more than one NOLINT on the line"); next }
  if (!match(line, directive)) {
    bad("not a directive of the form // NOLINT[NEXTLINE|BEGIN|END](<checks>): <reason>")
    next
  }
  text = substr(line, RSTART, RLENGTH)
  before = substr(line, 1, RSTART - 1)
  rest = substr(line, RSTART + RLENGTH)
  if (before ~ /\/$/) { bad("the directive must follow // exactly, not ///"); next }
  if (text ~ /^\/\/ NOLINT\(/) {
    # Code is what remains once leading block comments are set aside, unless a
    # line comment starts there, which makes the rest comment too.
    code = before
    # A */ before any /* closes a comment begun on an earlier line.
    if ((c = index(code, "*/")) > 0 && ((o = index(code, "/*")) == 0 || o > c)) code = substr(code, c + 2)
    while (sub(/^[ \t]*\/\*([^*]|\*+[^*\/])*\*+\//, "", code)) {}
    # A /* still left there is unclosed, so the directive sits inside it.
    if (code ~ /^[ \t]*$/ || code ~ /^[ \t]*\/[\/*]/) { bad("NOLINT must follow code on its line; put NOLINTNEXTLINE above the code instead"); next }
  } else if (before !~ /^[ \t]*$/) {
    bad("NOLINTNEXTLINE, NOLINTBEGIN and NOLINTEND must stand alone on their line")
    next
  }
  sub(/[ \t]+$/, "", rest)
  kind = text
  sub(/^\/\/ NOLINT/, "", kind)
  sub(/\(.*/, "", kind)
  list = text
  sub(/^[^(]*\(/, "", list)
  sub(/\)$/, "", list)
  if (kind == "END") {
    if (rest != "") { bad("NOLINTEND takes no reason"); next }
    if (depth == 0) { bad("NOLINTEND(" list ") closes no NOLINTBEGIN"); next }
    if (open_checks[depth] != list) { bad("NOLINTEND(" list ") closes NOLINTBEGIN(" open_checks[depth] ") from line " open_line[depth]); next }
    print open_key[depth] "\t#" h[depth] >> keys
    depth--
    hash_line(line)
    next
  }
  if (rest !~ /^: [A-Za-z]/) { bad("the directive needs a reason: \": <reason>\" starting with a letter"); next }
  n = split(list, names, ",")
  for (i = 1; i <= n; i++) print names[i] >> checks_out
  key = "nolint\t" file "\t" trim(line)
  if (kind == "") { print key "\t-" >> keys; hash_line(line) }
  else if (kind == "NEXTLINE") { pending = key; hash_line(line) }
  else {
    hash_line(line)
    depth++
    open_checks[depth] = list
    open_line[depth] = FNR
    open_key[depth] = key
    h[depth] = 0
  }
}
END { end_file() }
' {} +
touch "$work/nolint.keys" "$work/nolint.checks" "$work/nolint.errors"
if [[ -s $work/nolint.errors ]]; then
  die "malformed NOLINT directives:" "$(LC_ALL=C sort "$work/nolint.errors")" \
    "see CONTRIBUTING.md for the accepted forms"
fi

# --- Analysis ------------------------------------------------------------------

# run-clang-tidy takes regular expressions; name each source exactly, escaped.
file_patterns=()
while IFS= read -r s; do
  file_patterns+=("^$(printf '%s' "$s" | awk '{ gsub(/[][\\.^$*+?(){}|]/, "\\\\&"); print }')\$")
done <"$work/db-sources"
# run-clang-tidy exits nonzero when a source fails to compile; a count from a
# partial run is not a baseline, so stop. It runs from DIR because it reads
# the .clang-tidy of its working directory before analyzing.
if ! (cd "$root" && "$runner" -clang-tidy-binary "$tidy_path" -p "$build" -use-color=false \
  "${file_patterns[@]}") >"$work/raw" 2>&1; then
  grep -E ': (error|fatal error):' "$work/raw" >&2 || tail -20 "$work/raw" >&2
  die "clang-tidy failed; the count is not valid"
fi
# LLVM 21 prints `Running clang-tidy for M files out of N ...`, LLVM 23
# `Running clang-tidy in T threads for M files out of N ...`.
analyzed=$(awk '/^Running clang-tidy .*for [0-9]+ files out of/ { for (i = 1; i <= NF; i++) if ($i == "for") { print $(i + 1); exit } }' "$work/raw")
[[ $analyzed == "$nsources" ]] ||
  die "run-clang-tidy analyzed ${analyzed:-no} files, but $nsources sources are in the database"

# One `<path>\t<line>\t<column>\t<checks>\t<message>` line per distinct warning.
# A message can span lines, as a [[deprecated("...\n...")]] text does; its
# lines are joined with spaces until one ends with what reads as the [checks]
# list (see has_checks). A warning still without one when its source excerpt,
# the next diagnostic at a location or the output's end comes stops the
# script, since its checks cannot be told; a line such as "error: ..." with no
# location is read as part of the message. A message line that itself ends in
# such a list, as in "see [foo-bar]", is taken for it. The excerpt cannot mark
# where a message ends instead: clang omits it for some warnings, and
# clang-tidy can print a "note: this fix will not be applied ..." line between
# a message and it.
awk '
BEGIN {
  at = ":[0-9]+:[0-9]+: "
  warning = at "warning: "
}
# A check name is <module>-<name>, so it holds a `-`, and no space, `,` or `]`;
# clang-diagnostic ones can hold `+` and `#`, as in
# [clang-diagnostic-c++98-compat].
function has_checks(s) {
  return match(s, / \[[^] ,]*-[^] ,]*(,[^] ,]*-[^] ,]*)*\]$/)
}
function emit(line,    path, pos, lc, rest, checks, msg) {
  match(line, warning)
  path = substr(line, 1, RSTART - 1)
  pos = substr(line, RSTART + 1, RLENGTH - 1)
  sub(/: warning: $/, "", pos)
  split(pos, lc, ":")
  rest = substr(line, RSTART + RLENGTH)
  has_checks(rest)
  checks = substr(rest, RSTART + 2, RLENGTH - 3)
  msg = substr(rest, 1, RSTART - 1)
  gsub(/\t/, " ", msg)
  print path "\t" lc[1] "\t" lc[2] "\t" checks "\t" msg
}
function dangling() {
  print "  " pending > "/dev/stderr"
  failed = 1
  exit 1
}
# The next diagnostic at a location, or the source excerpt under this one
# (lines like "   12 | code" and "      | ^"), ends a message. A line opening
# with "note: " does not, since a message line can read so.
pending != "" && ($0 ~ (at "(warning|error|fatal error|note|remark): ") || /^ *[0-9]* \| /) { dangling() }
pending != "" {
  pending = pending " " $0
  if (has_checks(pending)) { emit(pending); pending = "" }
  next
}
$0 ~ warning {
  if (has_checks($0)) emit($0)
  else pending = $0
}
END {
  if (failed) exit 1
  if (pending != "") dangling()
}' "$work/raw" | LC_ALL=C sort -u >"$work/warnings.tsv" ||
  die "the warning above has no [checks] list, so it cannot be counted"

out="$build/clang-tidy-warnings.txt"
awk -F'\t' "${norm_vars[@]}" "$awk_lib"'
{ print norm($1) ":" $2 ":" $3 ": warning: " $5 " [" $4 "]" }
' "$work/warnings.tsv" | LC_ALL=C sort >"$out"

awk -F'\t' "${norm_vars[@]}" "$awk_lib"'
$1 != current {
  current = $1
  n = 0
  while ((getline l < current) > 0) lines[++n] = l
  close(current)
  for (i = n + 1; i in lines; i++) delete lines[i]
}
{ print "warning\t" norm($1) "\t" $4 "\t" $5 "\t" or_dash(trim(($2 <= n) ? lines[$2] : "")) }
' "$work/warnings.tsv" >"$work/warning.keys"

# One `suppressed\t<source>\t<count>` line per source.
awk "${norm_vars[@]}" -v sources="$work/db-sources" -v sq="'" "$awk_lib"'
function ends_with(s, t) { return length(s) >= length(t) && substr(s, length(s) - length(t) + 1) == t }
# shlex.quote: unchanged when every character is safe, else single-quoted with
# each embedded quote written as quote, double-quoted quote, quote.
function shell_quote(s,    q) {
  if (s !~ /[^A-Za-z0-9_@%+=:,.\/-]/) return s
  q = s
  gsub(sq, sq "\"" sq "\"" sq, q)
  return sq q sq
}
BEGIN { while ((getline s < sources) > 0) { list[++n] = s; count[s] = 0 } }
/^\[ *[0-9]+\/[0-9]+\]/ {
  # The header ends with the source: as it is in LLVM 21, and quoted as
  # Python'"'"'s shlex.quote does in LLVM 23.
  current = ""
  for (i = 1; i <= n; i++) {
    s = list[i]
    if (ends_with($0, s) || ends_with($0, shell_quote(s))) current = s
  }
  # A header naming no known source would leave its count unattributed.
  if (current == "") { print "unmatched run-clang-tidy header: " $0 > "/dev/stderr"; failed = 1; exit 1 }
  seen[current] = 1
  next
}
# For example `Suppressed 12 warnings (10 in non-user code, 1 NOLINT, 1 with check filters).`
current != "" && /^Suppressed [0-9]+ warnings \(/ && match($0, /[0-9]+ NOLINT[,)]/) {
  count[current] = substr($0, RSTART, RLENGTH - 8) + 0
}
END {
  if (failed) exit 1
  for (i = 1; i <= n; i++) if (!(list[i] in seen)) { print "no run-clang-tidy output for " list[i] > "/dev/stderr"; exit 1 }
  for (i = 1; i <= n; i++) print "suppressed\t" norm(list[i]) "\t" count[list[i]]
}
' "$work/raw" >"$work/suppressed.keys" ||
  die "could not attribute clang-tidy's suppression counts to the sources"

cat "$work/warning.keys" "$work/nolint.keys" "$work/suppressed.keys" | count_keys >"$work/current"

# --- Report --------------------------------------------------------------------

nwarnings=$(count_lines "$out")
ndirectives=$(count_lines "$work/nolint.keys")

case $mode in
  count)
    echo "$version"
    echo "total: $nwarnings"
    echo
    echo "by check:"
    awk '{ sub(/.*\[/, ""); sub(/\]$/, ""); print }' "$out" | rank_lines
    echo
    echo "by file:"
    cut -d: -f1 "$out" | rank_lines
    echo
    echo "NOLINT directives: $ndirectives"
    if [[ -s $work/nolint.checks ]]; then
      rank_lines <"$work/nolint.checks"
    fi
    echo
    echo "warnings: $out"
    ;;
  update)
    {
      echo "# Generated by scripts/tidy-count.sh --update; do not edit by hand."
      printf 'version\t%s\n' "$version"
      cat "$work/config"
      cat "$work/current"
    } >"$work/baseline"
    mv "$work/baseline" "$baseline"
    echo "wrote $baseline: $nwarnings warnings, $ndirectives NOLINT directives, clang-tidy: $version"
    ;;
  check)
    awk -F'\t' '$1 ~ /^[0-9]+$/' <"$baseline" >"$work/base-keys"
    diff=$(awk -F'\t' '
      { c = $1; k = substr($0, length($1) + 2) }
      NR == FNR { base[k] = c; keys[k] = 1; next }
      { cur[k] = c; keys[k] = 1 }
      END { for (k in keys) { d = cur[k] - base[k]; if (d != 0) printf "%+d\t%s\n", d, k } }
    ' "$work/base-keys" "$work/current" | LC_ALL=C sort -t "$(printf '\t')" -k2)
    if [[ -n $diff ]]; then
      printf 'error: clang-tidy results differ from %s:\n%s\n' "$baseline" "$diff" >&2
      if [[ -n $is_default ]]; then
        printf 'If the change is intended, run %s --update%s and commit the baseline;\n' "$0" "$rerun" >&2
        printf 'the pull request explains every key whose count rises.\n' >&2
      else
        printf 'This compared with %s, not the repository'"'"'s baseline. Explain every key\n' "$baseline" >&2
        printf 'whose count rises in the pull request; someone with the baseline'"'"'s\n' >&2
        printf 'environment updates .clang-tidy-baseline (see CONTRIBUTING.md).\n' >&2
      fi
      exit 1
    fi
    echo "clang-tidy results match $baseline: $nwarnings warnings, $ndirectives NOLINT directives"
    ;;
esac
