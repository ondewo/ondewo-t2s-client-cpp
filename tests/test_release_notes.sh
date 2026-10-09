#!/bin/sh
# The GitHub release body is sliced out of RELEASE.md by the Makefile's CURRENT_RELEASE_NOTES:
# a perl range from "Release ONDEWO <PRODUCT> C++ Client <version>" to the next ***** line. A
# heading spelled any other way gives an empty slice, and `gh release create -n ""` then
# publishes a release without notes and without an error; a range ending at /\*\*/ instead
# truncates the notes at their first **bold** span. This pins all of it.
#
# Product-agnostic (the product is read from the Makefile's ONDEWO_<PRODUCT>_VERSION), so the
# file is copied verbatim between the ONDEWO C++ clients. Registered with CTest by
# tests/CMakeLists.txt; run by hand as: sh tests/test_release_notes.sh <repository root>
set -eu

root="${1:-$(dirname "$0")/..}"
cd "$root"
failures=0
fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

product="$(sed -n 's/^ONDEWO_\([A-Z0-9]*\)_VERSION=.*/\1/p' Makefile | head -n 1)"
version="$(sed -n "s/^ONDEWO_${product}_VERSION=\(.*\)$/\1/p" Makefile | head -n 1)"
[ -n "$product" ] && [ -n "$version" ] || { echo "FAIL: no ONDEWO_<PRODUCT>_VERSION= in the Makefile" >&2; exit 1; }
heading="## Release ONDEWO ${product} C++ Client "

# 1. The Makefile slices exactly the heading pinned here, up to the ***** separator.
grep -qF "perl -ne 'print if /Release ONDEWO ${product} C\\+\\+ Client \${ONDEWO_${product}_VERSION}/../^\\*{5}/'" Makefile \
  || fail "CURRENT_RELEASE_NOTES is not the perl range /Release ONDEWO ${product} C\\+\\+ Client \${ONDEWO_${product}_VERSION}/../^\\*{5}/"

# 2. Every release heading uses that spelling and a MAJOR.MINOR.PATCH version.
grep -q "^## Release" RELEASE.md || fail "RELEASE.md has no '## Release' heading"
bad_headings="$(grep "^## Release" RELEASE.md | grep -v "^${heading}[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\$" || true)"
[ -z "$bad_headings" ] || fail "headings not spelled '${heading}X.Y.Z':
${bad_headings}"

# 3. Every section ends at a ***** separator before the next heading (or the end of the file).
unclosed="$(perl -ne '
  if (/^## Release/) { print "  before: $_" if $open; $open = 1 }
  $open = 0 if /^\*{5}/;
  END { print "  at the end of the file\n" if $open }
' RELEASE.md)"
[ -z "$unclosed" ] || fail "sections not closed by *****:
${unclosed}"

# 4. The Makefile's own variable yields non-empty notes for the version it releases.
probe="$(mktemp)"
printf '_print_current_release_notes:\n\t@printf "%%s\\n" "${CURRENT_RELEASE_NOTES}"\n' > "$probe"
notes="$(make -s --no-print-directory -f Makefile -f "$probe" _print_current_release_notes)"
rm -f "$probe"
first="$(printf '%s\n' "$notes" | head -n 1)"
last="$(printf '%s\n' "$notes" | tail -n 1)"
content="$(printf '%s\n' "$notes" | sed '1d;$d' | grep -c '[^[:space:]]' || true)"
[ "$first" = "${heading}${version}" ] || fail "the slice for ${version} starts with '${first}', not '${heading}${version}'"
printf '%s\n' "$last" | grep -q '^\*\*\*\*\*' || fail "the slice for ${version} does not end at a ***** separator"
[ "$content" -gt 1 ] || fail "the release notes of ${version} are empty"

[ "$failures" -eq 0 ] || { echo "${failures} release-notes check(s) failed" >&2; exit 1; }
echo "release notes OK: ${product} ${version}, every heading '${heading}X.Y.Z', every section closed by *****"
