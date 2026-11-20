#!/usr/bin/env bash
# Count clang-tidy warnings on TCAPI's sources in a form that is comparable
# from one run to the next.
#
# Usage: scripts/tidy-count.sh [BUILD_DIR]   (default: build-tidy)
#
# BUILD_DIR must be configured with CMAKE_EXPORT_COMPILE_COMMANDS=ON, as the
# brew-tidy preset is. The clang-tidy binary is taken from $CLANG_TIDY, else
# from TCAPI_CLANG_TIDY in BUILD_DIR's cache, else from PATH.
#
# The build log is not a usable count: an incremental build re-runs clang-tidy
# only on the sources it recompiles, and the compiler's own -W warnings are
# mixed into it. This script instead runs clang-tidy on every TCAPI source in
# the compilation database, then normalizes the output:
#   - a warning in a header is reported once per source that includes it, so
#     warnings are deduplicated by location, check and message;
#   - paths are made relative to the repository, and BUILD_DIR's own path is
#     written as <build>, so generated headers do not depend on its name;
#   - a warning listed under several aliases ([a,b]) counts once.
#
# It prints the clang-tidy version, the total and the counts per check and per
# file, and writes the deduplicated warnings, sorted, to
# BUILD_DIR/clang-tidy-warnings.txt.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$root/build-tidy}" && pwd)

if [[ ! -f "$build/compile_commands.json" ]]; then
  echo "error: $build/compile_commands.json not found; configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  exit 1
fi

tidy=${CLANG_TIDY:-}
if [[ -z "$tidy" ]]; then
  tidy=$(sed -n 's/^TCAPI_CLANG_TIDY:FILEPATH=//p' "$build/CMakeCache.txt" 2>/dev/null || true)
fi
tidy=${tidy:-clang-tidy}
tidy=$(command -v "$tidy")
runner="$(dirname "$tidy")/run-clang-tidy"
if [[ ! -x "$runner" ]]; then
  runner=$(command -v run-clang-tidy)
fi

raw=$(mktemp)
trap 'rm -f "$raw"' EXIT

# run-clang-tidy exits nonzero when a source fails to compile; a count from a
# partial run is not a baseline, so stop.
if ! "$runner" -clang-tidy-binary "$tidy" -p "$build" -quiet "^$root/source/" >"$raw" 2>&1; then
  grep -E ': (error|fatal error):' "$raw" >&2 || tail -20 "$raw" >&2
  echo "error: clang-tidy failed; the count is not valid" >&2
  exit 1
fi

out="$build/clang-tidy-warnings.txt"
grep -E '^[^ ]+:[0-9]+:[0-9]+: warning: .* \[[^]]+\]$' "$raw" \
  | awk -v build="$build/" -v root="$root/" '{
      if (index($0, build) == 1) { $0 = "<build>/" substr($0, length(build) + 1) }
      else if (index($0, root) == 1) { $0 = substr($0, length(root) + 1) }
      print
    }' \
  | LC_ALL=C sort -u >"$out"

"$tidy" --version | grep -i version | head -1
echo "total: $(wc -l <"$out" | tr -d ' ')"
echo
echo "by check:"
sed -E 's/.*\[([^]]+)\]$/\1/' "$out" | LC_ALL=C sort | uniq -c | LC_ALL=C sort -k1,1nr -k2
echo
echo "by file:"
cut -d: -f1 "$out" | LC_ALL=C sort | uniq -c | LC_ALL=C sort -k1,1nr -k2
echo
echo "warnings: $out"
