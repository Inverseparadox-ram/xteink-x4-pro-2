#!/usr/bin/env bash
#
# smerge -- pull the LATEST upstream CrossPlay release into this fork, keeping
# the fork's own apps and taking upstream's version of anything both touched.
#
#   ./scripts_local/smerge.sh            # to the newest upstream tag
#   ./scripts_local/smerge.sh v1.14.0    # to a named one
#
# This script does the MECHANICAL half and stops. It never resolves a conflict,
# never commits and never pushes: everything after the merge is judgement, and
# docs/smerge.md is the checklist for it.
#
# ---------------------------------------------------------------------------
# Why a graft, and why that is the whole trick.
#
# This fork's history begins at a tarball import with NO ancestry to upstream,
# so `git merge <upstream tag>` has no merge base and degenerates into an
# add/add conflict on every file in the tree -- thousands of them, each one a
# decision nobody can make correctly.
#
# But that import's tree IS upstream's release, near enough: at 1.13.2 it
# differed by five files the import never carried. So grafting the root commit
# onto the upstream tag it was imported FROM gives git a real merge base, and
# the same merge becomes a three-way one. Measured on the 1.13.2 -> 1.13.9 run:
# 325 files merged with no help at all and thirteen conflicts, eight of which
# were one app against its replacement.
#
# The graft is a local ref (refs/replace/...), is removed on every exit path by
# the trap below, and never reaches a commit or a push.
# ---------------------------------------------------------------------------
set -uo pipefail

REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)"
cd "$REPO"

REMOTE=crossplay
REMOTE_URL=https://github.com/ma-r-s/crossplay

die() {
  echo "smerge: $*" >&2
  exit 1
}

# --- The tree has to be clean, or the merge's conflicts are indistinguishable
# --- from whatever was already half-done.
[ -z "$(git status --porcelain)" ] || die "working tree is dirty. Commit or stash first."

# --- Where we are now. [crossplay] version in platformio.ini is the upstream
# --- release this fork currently carries; it is what the merge base has to be.
FROM_VERSION="$(awk '/^\[crossplay\]/{f=1;next} f&&/^version[[:space:]]*=/{print $3;exit}' platformio.ini)"
[ -n "$FROM_VERSION" ] || die "no [crossplay] version in platformio.ini; cannot tell what this tree is based on."
FROM_TAG="v${FROM_VERSION}"

git remote get-url "$REMOTE" >/dev/null 2>&1 || git remote add "$REMOTE" "$REMOTE_URL"

echo "smerge: fetching $REMOTE ..."
# Depth enough that the two tags connect: a shallow tag has no ancestry, and
# without ancestry there is no merge base even with the graft in place.
for attempt in 1 2 3; do
  if git fetch --depth 400 --tags "$REMOTE" >/dev/null 2>&1; then break; fi
  [ "$attempt" = 3 ] && die "could not fetch $REMOTE."
  sleep $((2 ** attempt))
done

# --- Where we are going.
if [ $# -ge 1 ]; then
  TO_TAG="$1"
else
  # Newest by version order, not by date: a patch release for an older line
  # can be tagged after a newer one.
  TO_TAG="$(git tag -l 'v*' --sort=-v:refname | head -1)"
fi
[ -n "$TO_TAG" ] || die "no upstream tags found."
git rev-parse -q --verify "${TO_TAG}^{commit}" >/dev/null || die "no such tag: $TO_TAG"
git rev-parse -q --verify "${FROM_TAG}^{commit}" >/dev/null || die "no such tag: $FROM_TAG (this tree's base)"

if [ "$TO_TAG" = "$FROM_TAG" ]; then
  echo "smerge: already on $TO_TAG -- nothing to merge."
  exit 0
fi

ROOT="$(git rev-list --max-parents=0 HEAD | tail -1)"
[ "$(git rev-list --max-parents=0 HEAD | wc -l)" = 1 ] ||
  die "this history has more than one root; the graft below would pick the wrong one."

cleanup() { git replace -d "$ROOT" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "smerge: $FROM_TAG -> $TO_TAG"
git replace --graft "$ROOT" "$FROM_TAG" >/dev/null 2>&1 || die "could not graft $ROOT onto $FROM_TAG."
BASE="$(git merge-base HEAD "$TO_TAG")" ||
  die "no merge base even with the graft: fetch depth is probably too shallow."
echo "smerge: merge base is $(git log --oneline -1 "$BASE")"

git merge --squash "$TO_TAG"
MERGE_RC=$?

# The submodule pointer is the trap that costs an hour. The merge stages
# upstream's new freeink-sdk commit, but the working tree still has the OLD one
# checked out -- so the next `git add -A` quietly stages the old pointer back
# and the build fails on SDK APIs that "should" exist.
SDK_WANT="$(git ls-tree "$TO_TAG" freeink-sdk | awk '{print $3}')"
SDK_HAVE="$(git -C freeink-sdk rev-parse HEAD 2>/dev/null)"

echo
echo "================================ smerge ================================"
if [ -n "$SDK_WANT" ] && [ "$SDK_WANT" != "$SDK_HAVE" ]; then
  echo "freeink-sdk MUST move: $SDK_HAVE"
  echo "                   ->  $SDK_WANT"
  echo "    git -C freeink-sdk fetch --depth 50 origin $SDK_WANT"
  echo "    git -C freeink-sdk checkout $SDK_WANT && git add freeink-sdk"
  echo "  Do this BEFORE any 'git add -A', which would stage the old one back."
  echo
fi
CONFLICTS="$(git diff --name-only --diff-filter=U)"
if [ -n "$CONFLICTS" ]; then
  echo "Conflicts to resolve by hand ($(echo "$CONFLICTS" | wc -l)):"
  echo "$CONFLICTS" | sed 's/^/    /'
else
  echo "No conflicts."
fi
echo
echo "Next: docs/smerge.md -- it names which side wins for each kind of file,"
echo "and the four guards whose counts have to be corrected afterwards."
echo "========================================================================"
exit "$MERGE_RC"
