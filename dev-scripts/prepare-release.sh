#!/bin/bash
# Prepares an iDared 32bit release:
#
# 1. Sets the app's version in iphone/Config.xcconfig, the only place it's
#    set, and its build number back to 1.
# 2. Renames the NEXT section of CHANGELOG-iDared32bit.md to the version.
#    If there isn't one, it adds a section drafted from the commits since the
#    last release's tag, for you to rewrite.
#
# It doesn't commit anything or create a tag; it tells you what to do next.
#
# Usage: dev-scripts/prepare-release.sh VERSION
# For example: dev-scripts/prepare-release.sh 1.3.23
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 VERSION  (for example: $0 1.3.23)" >&2
    exit 1
fi
VERSION="$1"
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: \"$VERSION\" isn't a version like 1.3.23." >&2
    exit 1
fi

cd "$(dirname "$0")/.."
XCCONFIG=iphone/Config.xcconfig
CHANGELOG=CHANGELOG-iDared32bit.md

OLD_VERSION=$(sed -n 's/^MARKETING_VERSION = //p' "$XCCONFIG")
if [ -z "$OLD_VERSION" ]; then
    echo "Error: $XCCONFIG doesn't set MARKETING_VERSION." >&2
    exit 1
fi
if grep -q "^## v$VERSION " "$CHANGELOG"; then
    echo "Error: $CHANGELOG already has a section for v$VERSION." >&2
    exit 1
fi

# 1. The version, and the build number, which starts again at 1.
awk -v version="$VERSION" '
    /^MARKETING_VERSION = / { print "MARKETING_VERSION = " version; next }
    /^CURRENT_PROJECT_VERSION = / { print "CURRENT_PROJECT_VERSION = 1"; next }
    { print }
' "$XCCONFIG" > "$XCCONFIG.tmp"
mv "$XCCONFIG.tmp" "$XCCONFIG"

# 2. The changelog section. NEXT, if it's there, becomes this version's.
HEADING="## v$VERSION ($(date +%Y-%m-%d))"
if grep -q '^## NEXT$' "$CHANGELOG"; then
    awk -v heading="$HEADING" '/^## NEXT$/ { print heading; next } { print }' \
        "$CHANGELOG" > "$CHANGELOG.tmp"
    mv "$CHANGELOG.tmp" "$CHANGELOG"
    CHANGELOG_DONE="Renamed the NEXT section of $CHANGELOG to v$VERSION."
    CHANGELOG_TODO="Check the v$VERSION section of $CHANGELOG."
else
    # Otherwise it's drafted from the commit subjects since the last iDared
    # 32bit release's tag (touchHLE's own tags are v0.x). Commits by anyone but
    # you (going by git's user.email) are credited, and fixup and version bump
    # commits are left out.
    LAST_TAG=$(git describe --tags --abbrev=0 --match 'v[1-9]*' 2>/dev/null || true)
    if [ -n "$LAST_TAG" ]; then
        RANGE="$LAST_TAG..HEAD"
        SINCE="since $LAST_TAG"
    else
        RANGE="HEAD"
        SINCE="in the whole history, as no earlier release tag was found"
    fi
    ME=$(git config user.email || true)
    DRAFT=$(git log --no-merges --reverse --format='%s%x09%an%x09%ae' "$RANGE" |
        awk -F '\t' -v me="$ME" '
            $1 ~ /^(fixup|squash|amend)! / { next }
            $1 ~ /^Set app version to / { next }
            { line = "- " $1; if ($3 != me) line = line " (by " $2 ")"; print line }
        ')
    COUNT=$(printf '%s' "$DRAFT" | grep -c '^- ' || true)

    SECTION=$(mktemp)
    {
        echo "$HEADING"
        echo
        echo "<!-- Draft of the commits $SINCE. Rewrite it for people using iDared"
        echo "32bit: group it under Compatibility, Usability, Documentation and Other,"
        echo "leave out what isn't worth mentioning, and don't name specific games for"
        echo "fixes. Then delete this comment. -->"
        echo
        if [ -n "$DRAFT" ]; then
            printf '%s\n' "$DRAFT"
        else
            echo "- (No commits found.)"
        fi
        echo
    } > "$SECTION"

    # Insert it before the first existing version section.
    awk -v section="$SECTION" '
        !inserted && /^## / {
            while ((getline line < section) > 0) print line
            inserted = 1
        }
        { print }
        END { if (!inserted) while ((getline line < section) > 0) print line }
    ' "$CHANGELOG" > "$CHANGELOG.tmp"
    mv "$CHANGELOG.tmp" "$CHANGELOG"
    rm -f "$SECTION"
    CHANGELOG_DONE="Added a v$VERSION section to $CHANGELOG, drafted from $COUNT commit(s) $SINCE."
    CHANGELOG_TODO="Rewrite the v$VERSION section of $CHANGELOG."
fi

cat <<EOF
Set iDared 32bit's version to $VERSION, build 1 (it was $OLD_VERSION), in $XCCONFIG.
$CHANGELOG_DONE

Next:
  1. $CHANGELOG_TODO
  2. Commit: git commit -am "Set app version to $VERSION (Build 1)"
  3. Build it and submit it with Xcode.
  4. Tag the commit you built, and push the tag:
       git tag -a v$VERSION -m "iDared 32bit $VERSION" -m "Source for iDared 32bit $VERSION. The App Store build ($VERSION, build 1) was built from this code." -m "Licensed under the Mozilla Public License 2.0."
       git push <remote> v$VERSION
EOF
