#!/usr/bin/env bash
# Count clang-tidy warnings on TCAPI's sources in a form that is comparable
# from one run to the next, and compare them with the committed baseline.
#
# Usage: scripts/tidy-count.sh [--check | --update] [--baseline FILE]
#                              [--root DIR] [BUILD_DIR]
#
#   (no mode)  print the counts
#   --update   write the baseline FILE from the current warnings
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
# mixed into it. This script instead re-runs CMake on BUILD_DIR, so that its
# compile_commands.json and generated headers match the sources, then runs
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
# Every line under DIR/source and DIR/include containing the text NOLINT must
# be one directive in one of these forms, because clang-tidy honors that text
# anywhere on a line, in many spellings:
#   // NOLINT(<checks>): <reason>
#   // NOLINTNEXTLINE(<checks>): <reason>
#   // NOLINTBEGIN(<checks>): <reason>
#   // NOLINTEND(<checks>)
# NOLINT follows the code it applies to; the other three stand alone on their
# line. <checks> is a comma-separated list of check names (letters, digits, `.`, `_`
# and `-`) without spaces or `*`, <reason> starts with a letter, and each
# NOLINTEND closes the innermost open NOLINTBEGIN, which must name the same
# checks.
#
# Run by pre-commit at pre-push, the script also requires that the pushed
# commit is HEAD and that the working tree has no change pre-commit leaves in
# place, so that what it analyzes is what is pushed.

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

# Prints the physical path of the directory $1, symbolic links resolved.
physpath() {
  (CDPATH='' cd -- "$1" 2>/dev/null && pwd -P)
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
      (($# >= 2)) || die "$1 needs an argument"
      if [[ $1 == --baseline ]]; then baseline=$2; else root=$2; fi
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    -*) die "unknown option: $1" "run with --help for usage" ;;
    *)
      [[ -z $build_arg ]] || die "more than one BUILD_DIR: $build_arg, $1"
      build_arg=$1
      ;;
  esac
  shift
done

script_repo=$(abspath "$(dirname "$0")/..")
if [[ -n $root ]]; then
  [[ -d $root ]] || die "source tree $root not found"
  root=$(abspath "$root")
else
  root=$script_repo
fi
baseline=${baseline:-$script_repo/.clang-tidy-baseline}
build_arg=${build_arg:-$root/build-tidy}
[[ -d $build_arg ]] ||
  die "build directory $build_arg not found" "configure it with: cmake --preset brew-tidy"
build=$(abspath "$build_arg")
[[ -f $build/CMakeCache.txt ]] ||
  die "$build is not a CMake build directory" "configure it with: cmake --preset brew-tidy"

tidy=${CLANG_TIDY:-}
if [[ -z $tidy ]]; then
  tidy=$(awk -F= '/^TCAPI_CLANG_TIDY:FILEPATH=/ { print $2 }' "$build/CMakeCache.txt")
fi
tidy=${tidy:-clang-tidy}
tidy_path=$(command -v "$tidy" || true)
[[ -n $tidy_path ]] || die "clang-tidy not found: $tidy" "install it with: brew install llvm"
runner="$(dirname "$tidy_path")/run-clang-tidy"
[[ -x $runner ]] || runner=$(command -v run-clang-tidy || true)
[[ -n $runner ]] || die "run-clang-tidy not found" "install it with: brew install llvm"
version=$("$tidy_path" --version | awk 'tolower($0) ~ /version/ { sub(/^[ \t]+/, ""); print; exit }')

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Awk helpers shared by the programs below: path normalization and trimming.
# norm() takes the two prefixes it rewrites from norm_vars, set once the
# database's spelling of DIR and BUILD_DIR is known.
awk_lib='
function norm(p) {
  if (index(p, build) == 1) return "<build>/" substr(p, length(build) + 1)
  if (index(p, root) == 1) return substr(p, length(root) + 1)
  return p
}
function trim(s) {
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
  $0 --update --baseline <file> --root <dir> <dir's build directory>
  $0 --check --baseline <file> <this tree's build directory>
CONTRIBUTING.md describes this. To push without the check: SKIP=clang-tidy-baseline git push
EOF
}

# --- Checks that need no analysis --------------------------------------------

if [[ $mode == check ]]; then
  [[ -f $baseline ]] || die "baseline $baseline not found" "write it with: $0 --update"
  base_version=$(awk -F'\t' '$1 == "version" { print $2; exit }' "$baseline")
  if [[ $base_version != "$version" ]]; then
    printf 'error: clang-tidy version differs from the baseline; nothing was compared\n' >&2
    printf '  baseline: %s\n  running:  %s\n' "$base_version" "$version" >&2
    fallback_hint
    exit 1
  fi
fi

if [[ ${PRE_COMMIT:-} == 1 ]]; then
  [[ -n ${PRE_COMMIT_TO_REF:-} ]] ||
    die "pre-commit did not say which commit is pushed, as for a push that reaches a root commit" \
      "run $0 --check by hand, then push with SKIP=clang-tidy-baseline"
  # A pushed annotated tag arrives as the tag object; compare the commit.
  pushed=$(git -C "$root" rev-parse "$PRE_COMMIT_TO_REF^{commit}")
  [[ $pushed == "$(git -C "$root" rev-parse HEAD)" ]] ||
    die "the pushed commit $pushed is not HEAD, which is what this check analyzes" \
      "check out the branch you push, and push one branch at a time"
  dirty=$(git -C "$root" status --porcelain)
  [[ -z $dirty ]] ||
    die "the working tree differs from HEAD, which is what is pushed:" "$dirty" \
      "commit, stash or remove these changes before pushing"
fi

# --- Configure, and check the compilation database ---------------------------

home=$(awk -F= '/^CMAKE_HOME_DIRECTORY:INTERNAL=/ { print $2 }' "$build/CMakeCache.txt")
[[ -n $home && $(physpath "$home") == "$(physpath "$root")" ]] ||
  die "$build was configured from ${home:-an unknown tree}, not from $root"
# compile_commands.json and clang-tidy spell paths the way CMake was configured
# with them, which differs from DIR and BUILD_DIR when CMake reached the same
# directories by another path, such as through a symbolic link, so match
# against CMake's spelling from here on.
root=$home
cachedir=$(awk -F= '/^CMAKE_CACHEFILE_DIR:INTERNAL=/ { print $2 }' "$build/CMakeCache.txt")
[[ -n $cachedir && $(physpath "$cachedir") == "$(physpath "$build")" ]] && build=$cachedir
norm_vars=(-v "build=$build/" -v "root=$root/")
if ! cmake "$build" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$work/cmake.log" 2>&1; then
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
index($1, p) == 1 {
  n = split($2, t, " ")
  opts = ""
  for (i = 1; i <= n; i++) {
    if (t[i] ~ /^-[DU]./ || t[i] ~ /^-std=/) opts = opts " " t[i]
    else if ((t[i] == "-D" || t[i] == "-U" || t[i] == "-include" || t[i] == "-imacros") && i < n) { opts = opts " " t[i] " " t[i + 1]; i++ }
  }
  sub(/^ /, "", opts)
  print "config\t" norm($1) "\t" replace_all(opts, root, "<root>/")
}' "$work/db.tsv" | LC_ALL=C sort -u >"$work/config"

if [[ $mode == check ]]; then
  awk -F'\t' '$1 == "config"' "$baseline" | LC_ALL=C sort -u >"$work/base-config"
  mismatch=$(awk -F'\t' '
    NR == FNR { base[$2] = $3; next }
    ($2 in base) && base[$2] != $3 { printf "  %s\n    baseline: %s\n    current:  %s\n", $2, base[$2], $3 }
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
find "$root/source" "$root/include" -type f -exec env LC_ALL=C awk \
  "${norm_vars[@]}" \
  -v keys="$work/nolint.keys" -v checks_out="$work/nolint.checks" -v errors="$work/nolint.errors" \
  "$awk_lib"'
BEGIN {
  for (i = 1; i < 256; i++) ord[sprintf("%c", i)] = i
  directive = "// NOLINT(NEXTLINE|BEGIN|END)?\\([A-Za-z0-9_.-]+(,[A-Za-z0-9_.-]+)*\\)"
}
function bad(msg) { print file ":" FNR ": " msg >> errors }
function end_file() {
  if (file == "") return
  if (pending != "") { print pending "\t" >> keys; pending = "" }
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
  if (pending != "") { print pending "\t" trim($0) >> keys; pending = "" }
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
    if (before ~ /^[ \t]*$/) { bad("NOLINT must follow code on its line; put NOLINTNEXTLINE above the code instead"); next }
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
  if (kind == "") { print key "\t" >> keys; hash_line(line) }
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

pattern=$(printf '%s' "$root" | awk '{ gsub(/[][\\.^$*+?(){}|]/, "\\\\&"); print }')
# run-clang-tidy exits nonzero when a source fails to compile; a count from a
# partial run is not a baseline, so stop. It runs from DIR because it reads
# the .clang-tidy of its working directory before analyzing.
if ! (cd "$root" && "$runner" -clang-tidy-binary "$tidy_path" -p "$build" -use-color=false \
  "^$pattern/source/") >"$work/raw" 2>&1; then
  grep -E ': (error|fatal error):' "$work/raw" >&2 || tail -20 "$work/raw" >&2
  die "clang-tidy failed; the count is not valid"
fi
# LLVM 21 prints `Running clang-tidy for M files out of N ...`, LLVM 23
# `Running clang-tidy in T threads for M files out of N ...`.
analyzed=$(awk '/^Running clang-tidy .*for [0-9]+ files out of/ { for (i = 1; i <= NF; i++) if ($i == "for") { print $(i + 1); exit } }' "$work/raw")
[[ $analyzed == "$nsources" ]] ||
  die "run-clang-tidy analyzed ${analyzed:-no} files, but $nsources sources are in the database"

# One `<path>\t<line>\t<column>\t<checks>\t<message>` line per distinct warning.
awk '
match($0, /:[0-9]+:[0-9]+: warning: /) {
  path = substr($0, 1, RSTART - 1)
  pos = substr($0, RSTART + 1, RLENGTH - 1)
  sub(/: warning: $/, "", pos)
  split(pos, lc, ":")
  rest = substr($0, RSTART + RLENGTH)
  if (!match(rest, / \[[^]]+\]$/)) next
  checks = substr(rest, RSTART + 2, RLENGTH - 3)
  msg = substr(rest, 1, RSTART - 1)
  gsub(/\t/, " ", msg)
  print path "\t" lc[1] "\t" lc[2] "\t" checks "\t" msg
}' "$work/raw" | LC_ALL=C sort -u >"$work/warnings.tsv"

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
{ print "warning\t" norm($1) "\t" $4 "\t" $5 "\t" trim(($2 <= n) ? lines[$2] : "") }
' "$work/warnings.tsv" >"$work/warning.keys"

# One `suppressed\t<source>\t<count>` line per source.
awk "${norm_vars[@]}" -v sources="$work/db-sources" "$awk_lib"'
BEGIN { while ((getline s < sources) > 0) { list[++n] = s; count[s] = 0 } }
/^\[ *[0-9]+\/[0-9]+\]/ {
  # The header ends with the source, quoted when its path has a space.
  header = $0
  sub(/'"'"'$/, "", header)
  current = ""
  for (i = 1; i <= n; i++) {
    s = list[i]
    if (length(header) >= length(s) && substr(header, length(header) - length(s) + 1) == s) current = s
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
    awk -F'\t' '$1 ~ /^[0-9]+$/' "$baseline" >"$work/base-keys"
    diff=$(awk -F'\t' '
      { c = $1; k = substr($0, length($1) + 2) }
      NR == FNR { base[k] = c; keys[k] = 1; next }
      { cur[k] = c; keys[k] = 1 }
      END { for (k in keys) { d = cur[k] - base[k]; if (d != 0) printf "%+d\t%s\n", d, k } }
    ' "$work/base-keys" "$work/current" | LC_ALL=C sort -t "$(printf '\t')" -k2)
    if [[ -n $diff ]]; then
      printf 'error: clang-tidy results differ from %s:\n%s\n' "$baseline" "$diff" >&2
      printf 'If the change is intended, run %s --update and commit the baseline;\n' "$0" >&2
      printf 'the pull request explains every key whose count rises.\n' >&2
      exit 1
    fi
    echo "clang-tidy results match $baseline: $nwarnings warnings, $ndirectives NOLINT directives"
    ;;
esac
