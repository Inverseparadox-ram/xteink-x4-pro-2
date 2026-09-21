# smerge

Pull the latest upstream CrossPlay release into this fork, keep the fork's own
apps, and take upstream's version of anything both touched.

```bash
./scripts_local/smerge.sh            # to the newest upstream tag
./scripts_local/smerge.sh v1.14.0    # to a named one
```

The script does the mechanical half and stops at the conflicts. This page is
the rest.

## Why it is a graft and not a merge

This fork's history begins at a tarball import with **no ancestry to upstream**,
so a plain `git merge <tag>` has no merge base: every file in the tree comes
back as an add/add conflict. Thousands of decisions, none of them answerable.

The import's tree *is* an upstream release, near enough -- at 1.13.2 it differed
by five files the import never carried. Grafting the root commit onto the tag
it was imported from gives git a real merge base, and the same command becomes
a three-way merge.

Measured on the 1.13.2 → 1.13.9 run: **325 files merged unaided, 13 conflicts**,
eight of which were one app against its replacement. The graft is a local
`refs/replace/` ref, removed on every exit path, and never reaches a commit.

`[crossplay] version` in `platformio.ini` is what tells the script which tag
this tree is currently based on. It is upstream's own release number, so after
a successful smerge the merge itself updates it -- nothing to bump by hand.

## Which side wins

| File | Take | Why |
| --- | --- | --- |
| `src/apps_local/<our app>/` | **ours** | Clock, Weather, Remote are the reason the fork exists |
| Anything else under `lib/`, `src/`, `freeink-sdk` | **theirs** | we do not maintain the reader |
| An app we both wrote | **theirs**, wholesale | see below |
| `Shelf.cpp` kApps table | both, **ours appended after theirs** | a sync then lands as an append, not a reshuffle |
| `ToyboxIcons.h`, `icons.txt` | both, minus marks our side no longer references | |
| README / site / `docs/buttons.md` counts | neither -- **recompute** | the guards derive them from the tree |

**When upstream ships an app we already had, theirs replaces ours entirely.**
Not merged -- replaced. At 1.13.9 both had a Notes app and they shared a name
and nothing else: his API is a deck of lists
(`buildDeck`/`buildNote`/`buildMenu`/`buildPhone`), ours was a list and a
checklist. There was nothing to carry across. Delete all of ours: sources,
`host-tests/<app>/`, its blocks in `host-tests/ui/test_ui.cpp`, its entry in
`host-tests/ui/run.sh`, its icons in `ToyboxIcons.h`, its `icons.txt` stanza,
its README row, its site card and its `docs/apps/` page.

## The three traps

**1. The submodule pointer.** The merge stages upstream's new `freeink-sdk`
commit, but the working tree still has the old one checked out -- so the next
`git add -A` stages the old pointer straight back, and the build then fails on
SDK APIs that ought to exist. Move the submodule *before* any `git add -A`:

```bash
git -C freeink-sdk fetch --depth 50 origin <sha>
git -C freeink-sdk checkout <sha> && git add freeink-sdk
```

The script prints the exact sha. At 1.13.9 the old SDK could not compile the
release at all (`ThemeTokens` gained `listMinRowHeight`, `GfxRendererTarget` a
second constructor argument), so this is not optional.

**2. The simulator shim.** `lib/hal/` is the device HAL; the simulator ships its
own copy. When upstream adds a HAL method our apps call, the simulator build
breaks. The fix goes in `scripts_local/sim_catchup.py`, which patches the shim
at build time -- that is what it is for, and upstream already uses it for
`setTimezone()`. Do not `#ifdef SIMULATOR` around the call site.

**3. The counts four guards derive from the tree.** Do not reason these out;
resolve the conflict either way, run the guards, and copy the numbers they
print.

```bash
bash host-tests/docsclaims/run.sh        # README '### Apps' rows vs kApps;
                                          # docs/buttons.md button census
python3 host-tests/site/shelf_coverage.py .   # every kApps title on site + README
bash host-tests/site/run.sh              # card structure
```

## Finishing

```bash
./bin/clang-format-fix -g
./scripts_local/check.sh
```

Read the verdict with `grep -o 'CHECKSH-VERDICT: [a-z-]*'`, never `tail -1`.

**Upstream ships with some suites already red**, so a red gate is not by itself
a failure of the merge. Establish the baseline rather than assuming it: check
out the release in a scratch clone and run the suites that failed.

```bash
git -C /path/to/clean/crossplay checkout v1.14.0
git -C /path/to/clean/crossplay submodule update --init --recursive
bash host-tests/<suite>/run.sh
```

At 1.13.9 four failed -- `bugflow`, `forkoverrides`, `nodash`, `wikipedia` -- and
all four failed byte-identically on a clean checkout. A suite that fails here
and passes there is ours to fix.

Then the image, which is the only thing that reaches the device:

```bash
pio run -e gh_release_x4pro
cp .pio/build/gh_release_x4pro/firmware.factory.bin dist/crossplay-<ver>-<apps>-x4pro-full.bin
./scripts_local/verify-image.py .pio/build/gh_release_x4pro dist/<that file> partitions.csv
```

31 checks, 0 failed, or it does not ship.

## Watch the flash budget

`check.sh` prints the app slot's percentage on every run. 1.13.2 + our three
apps was 85.4%; 1.13.9 + the same three is **87.0%** -- upstream's own growth,
not ours. See [docs/flash-budget.md](flash-budget.md) for what to cut when it
gets close, and what happens if it does not.
