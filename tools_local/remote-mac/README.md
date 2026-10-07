# CrossPlay Unlock (the Mac half)

The reader's unlock button needs something on the Mac to answer it. This is
that something: a small agent that verifies the reader's challenge, checks
whether the screen is really locked, and sends the password back encrypted.

Everything here runs on the Mac. Nothing in this folder is built by the
firmware build, and `check.sh` does not touch it -- it cannot, because Swift,
CoreBluetooth and the macOS SDK are not on the reader's build host. It has
been built and run against a real Mac mini and a real reader; see the log
below for what that looked like.

## What it does, and what it deliberately does not

macOS exposes **no API for dismissing the lock screen**. Not to a signed
helper, not to a LaunchAgent, not to anything: Apple reserves unlocking for the
password field, Touch ID and Apple Watch. So the only way any Bluetooth device
unlocks a Mac is to **type the password**, and that is what the reader does.

Given that, the exchange buys three things:

1. **The reader holds no password.** It lives in this Mac's Keychain. This
   agent sends it -- encrypted under a key derived from the paired secret and a
   nonce that has never been used before -- and only in answer to a request it
   has authenticated.
2. **The reader will not type into an unlocked session.** This agent reports
   the real lock state inside the MAC, and sends no password at all unless the
   screen is locked. Without that, pressing unlock at an awake Mac puts the
   password into whatever field has focus.
3. **Only the paired reader is answered.** A request signed with anything but
   the paired secret shows up here as a bad MAC and is not answered. Five in a
   row start a pause that doubles from 30 seconds; **ten in a row lock the
   helper out** until you run `crossplay-unlock unblock`. The count is kept on
   disk, so restarting the helper does not reset it.

The reader unlocks on **one tap**, with nothing entered on it, so the reader
itself is the key: anyone holding it near this Mac while you are logged in and
locked can unlock it. The secret is kept in the reader's internal flash rather
than on its SD card, so copying it takes the reader, a cable and flashing
tools.

It does nothing at the FileVault pre-boot screen: Bluetooth is not up that
early, so a Mac that has been powered off needs its keyboard.

It also **mutes every microphone** the Mac has when the reader's microphone
button asks: the mute control on devices that have one, the input volume set
to zero on those that do not, and anything that comes back on while the mute
is wanted is turned back down within five seconds. It **cannot touch the
camera**: macOS has no supported way for a program to switch one off.

It also tells the reader **what is playing**: the title and artist from Music
or Spotify, which both broadcast a notification on every change. That needs no
extra permission. It cannot see a browser playing YouTube -- nothing on current
macOS will tell a third-party process about that -- and it learns a song only
at the first play, pause or track change after it starts.

## Install

```sh
./build.sh
sudo mkdir -p /usr/local/bin
sudo rm -f /usr/local/bin/crossplay-unlock
sudo cp crossplay-unlock /usr/local/bin/
crossplay-unlock pair
```

`./build.sh` needs Xcode's command line tools (`xcode-select --install`), and
`pair` asks for the reader's code and then this Mac's password.

No `#` comments on the command lines here, and none anywhere else in this file,
because these get pasted into a shell rather than read. **zsh does not treat
`#` as a comment when it is interactive** -- `interactive_comments` is off by
default -- so a commented `sudo mkdir -p /usr/local/bin` creates directories
called `#`, `Apple` and `Silicon` in whatever folder you are standing in.

The `rm` before the `cp` is not tidiness. macOS caches a program's code
signature against the file itself, so copying a new build over the old one
leaves a cached signature that describes the old bytes, and the kernel kills
the new program the moment it starts: `Killed: 9`, with no other message.
Removing it first makes the copy a new file with nothing cached against it.

`/usr/local/bin` is on the default PATH (`/etc/paths` lists it) but nothing
creates it on a Mac whose Homebrew lives in `/opt/homebrew`, so the copy fails
with "No such file or directory" until you make it. Keep that path:
`com.crossplay.unlock.plist` names the binary there.

Run it once in Terminal before installing the agent:

```sh
crossplay-unlock run
```

The first run is when macOS asks for Bluetooth permission, and an agent started
by launchd has no way to ask. Answer yes, check the log says `listening for
challenges`, then stop it and install the agent:

```sh
cp com.crossplay.unlock.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.crossplay.unlock.plist
tail -f /tmp/crossplay-unlock.log
```

`bootstrap`, not `load`: the old subcommand reports almost every failure as
"Load failed: 5: Input/output error", including a plist that is not there.
To take it back out: `launchctl bootout gui/$(id -u)/com.crossplay.unlock`.

A LaunchAgent keeps running behind the lock screen, which is the whole reason
it is an agent and not a login item.

## Updating

After the first install, one command brings it up to date -- pull, build,
install, restart, and print what the helper reports. From this folder,
wherever you cloned it:

```sh
./update.sh
```

It works from any directory if you give the full path. `mdfind -name
CrossPlayUnlock.swift` finds this folder if you have lost track of it.

It asks for your Mac password once, for the copy into `/usr/local/bin`.

## Pairing

On the reader: open **Remote** and press the padlock. With nothing paired it
shows a 32-character code in eight groups of four, **once**. Type that into
`crossplay-unlock pair`, then press either side key on the reader. That is the
whole pairing.

`pair` prints back the code it understood, grouped as the reader shows it.
**Compare them.** Every well-formed code is a valid secret, so a mistyped
character is not an error anywhere -- it is a pairing that never answers. The
running helper picks the new pairing up by itself; nothing needs restarting.

A reader paired under a combination or a touch PIN, by an earlier build, has to
pair again once. So does a reader that has had a USB full flash, which erases
the internal flash the pairing lives in.

Re-pairing generates a **new secret** on the reader and starts its counter over
at zero, so `pair` resets the replay ledger here to match. It has to: the old
high-water mark belongs to a conversation that no longer exists, and left in
place it refuses every challenge the new pairing sends until the reader climbs
back past it. The reader has no way to be told that, so the symptom is an
unlock button that verifies fine and never works.

## Commands

| | |
| --- | --- |
| `crossplay-unlock pair` | store the reader's code and this Mac's password |
| `crossplay-unlock password` | store a new password, keeping the pairing and its counter |
| `crossplay-unlock unblock` | lift the lockout after ten unverifiable requests, keeping the pairing |
| `crossplay-unlock status` | what it has, and whether the screen is locked right now |
| `crossplay-unlock forget` | delete both from the Keychain |
| `crossplay-unlock run` | serve challenges; what launchd runs |
| `crossplay-unlock claude-setup` | add the hook that feeds the reader's CLAUDE page to `~/.claude/settings.json` (once) |
| `crossplay-unlock services` | print what each line of `services.txt` finds right now |
| `crossplay-unlock adopt NAME --run "CMD"` | keep a command running under launchd (also `--app`, `--docker`, `--launchd`) |
| `crossplay-unlock unadopt NAME` | stop keeping it running and take it off the MAC page |
| `crossplay-unlock tv [up\|down [N]]` | show the TV volume commands, or run one now (see "The TV's volume") |
| `crossplay-unlock doctor` | print the Service doctor's session id, to open that chat yourself |
| `crossplay-unlock claude-hook` | what Claude Code runs on each event; not for typing |

## The CLAUDE and MAC pages

The reader's remote has three pages, stepped through by the arrow on its band:
the controls, **CLAUDE** and **MAC**.

**CLAUDE** lists the Claude Code sessions on this Mac with one word each:
*in process*, *awaiting input*, *completed* or *failed*. It is fed by Claude
Code's own hooks, installed once:

    crossplay-unlock claude-setup

That adds `crossplay-unlock claude-hook` to `~/.claude/settings.json` for
UserPromptSubmit, PreToolUse (AskUserQuestion and ExitPlanMode only),
PostToolUse, Notification, Stop and SessionEnd, keeping everything already in
the file and a copy of it as `settings.json.crossplay-backup`. Each event
rewrites one line of `claude-sessions.json` beside the ledger, and the agent
reads it when the reader asks (below). A session is *awaiting input* when Claude asks a
question, wants a permission or waits on a plan; *completed* when its turn
ends; *failed* when its `claude` process disappears mid-turn. Completed rows
leave after a day, failed after two hours, closed sessions at once. The title
is the one Claude Code gave the session, else its first prompt. Sessions
already open before the setup appear from their next prompt.

Both pages are **pulled**: the reader asks for a list when the page opens,
when the Mac helper reconnects while it is open, and when REFRESH is tapped
(`[1, 4, board]` on the command characteristic), and the helper answers with
the whole list, changed or not. Nothing is checked or sent while nobody is
looking, and nothing is sent unasked, a restart's progress included: the
reader pulls the MAC board once right after RESTART (the row then reads
*restarting*), and REFRESH shows *restarted*, *asking Claude* or Claude's
answer as they come. The page says when it was last updated, or that the Mac did not answer within
eight seconds.

**MAC** lists your own background services from `services.txt` beside the
ledger (`~/Library/Application Support/CrossPlayUnlock/`), one per line as
`NAME | CHECK | START`, checked when the reader asks. A check is
`launchd <label>`, `process <text>` (a command line containing it),
`docker <container>`, `http <url>` or `self`. START is optional: the shell
command that starts the service again (see below). The file is written on
first run with guesses for Ambient tasks, Immich, Voice typing, Wake TV and
Remote unlock: **edit the checks to match how each actually runs here**, then
run `crossplay-unlock services` to see what each finds.

## Keeping services running

Three layers, cheapest first.

**1. macOS keeps it alive (`adopt`).** Most of what goes down on a Mac is a
process that exited, and launchd will restart one by itself if it is told to.
`adopt` writes a launchd agent with `KeepAlive` for it, loads it, and points
the service's line in `services.txt` at it:

    crossplay-unlock adopt "Voice typing" --run "/usr/local/bin/voice-typer --serve"
    crossplay-unlock adopt "Wake TV"      --app "Wake TV"
    crossplay-unlock adopt "Immich"       --docker immich_server
    crossplay-unlock adopt "Ambient"      --launchd com.me.ambient

- `--run` runs the command through a login shell, so it sees Terminal's PATH.
  **It must stay in the foreground**: a command that forks and exits looks like
  a crash to launchd and is started again every 20 seconds. Stop whatever
  started it before (a login item, a `&` in a script) first, or there will be
  two.
- `--app` keeps an app open (`open -W -a`), so quitting it reopens it.
- `--docker` sets the container's restart policy to `unless-stopped`, so the
  engine restarts it when it dies and when the engine starts. **OrbStack and
  Docker Desktop both work**; the helper uses OrbStack's `docker`
  (`~/.orbstack/bin`) when OrbStack is installed. No launchd agent: OrbStack
  (Settings > Start at login) or Docker Desktop has to start at login itself.
- `--launchd` adopts a job that already exists, for the restart button below.

Agents are `~/Library/LaunchAgents/com.crossplay.svc.<name>.plist`, and their
output goes to `logs/<name>.log` beside the ledger. `unadopt NAME` removes the
agent and the line.

**2. RESTART on the reader.** A service on the MAC page that is *stopped*,
*failed* or *unknown* can be tapped; the reader asks for a confirmation, then
sends the helper the row and a check byte of its name (so a list that changed
in the meantime cannot restart the wrong thing). The helper runs the line's
START, or for `launchd` and `docker` lines one it knows:
`launchctl kickstart -k`, falling back to `launchctl bootstrap`; and for
`docker`, the engine first if it is down (`orb start` for OrbStack, else the
app is opened; up to two minutes for it to answer), then the container, and
when it belongs to a Compose project (Immich's does) every container in that
project, so the server does not come back to a stopped database. Every
recipe runs with a terminal's PATH (launchd's has no `docker`), and a START of
your own can call `crossplay_engine_up` for the same engine step. A `docker`
row reads *OrbStack off* when the engine is down and *no container NAME* when
the name is wrong. The row reads *restarting* meanwhile, and *restarted* if it is
back eight seconds later; the reader sees each when it pulls (REFRESH).

**3. The Service doctor.** If the restart did not bring it back, the helper
asks Claude Code, headless (`claude -p`), with what the check says, the start
command and what it printed. The row reads *asking Claude*, and then Claude's
last line (at most 30 characters) for half an hour.

Every one of these runs **resumes the same session**, so they are all one
chat -- the Service doctor -- which remembers what it tried last time. Its id
is in `doctor-session.txt`; `crossplay-unlock doctor` prints it, and

    claude --resume <id>

opens it in a terminal to read or carry on. The full answers are also in
`logs/doctor.log`. If you ran `claude-setup` it shows on the reader's CLAUDE
page too, like any other session. If the saved session cannot be resumed
(cleared, or copied from another Mac), a new one is started and saved.

Nobody is at the Mac to approve anything, so the doctor runs with a fixed list
of tools it may use without asking: reading files, and `launchctl`, `docker`,
`open`, `ps`, `pgrep`, `pkill`, `lsof`, `tail`, `cat`, `ls`, `log show`,
`brew services`, `curl` and this helper. Put rules one per line in
`doctor-tools.txt` beside the ledger (the same syntax as `--allowedTools`, e.g.
`Bash(npm run:*)`) and that list replaces the default one. It runs in the
`doctor/` folder beside the ledger, finds `claude` in `~/.local/bin`,
`~/.claude/local`, Homebrew or your login shell's PATH, and is stopped after
ten minutes.

## The TV's volume

The reader's - and + can drive a TV instead of the Mac: the helper runs a
command of yours for each press, typically the Python script that talks to an
LG webOS TV. Edit `tv.txt` beside the ledger (written with instructions the
first time the helper runs):

    up   = python3 ~/lgtv/lgtv.py volume up
    down = python3 ~/lgtv/lgtv.py volume down

- Each runs from your home folder with a terminal's PATH, so `~` paths and
  Homebrew's `python3` work; give a venv's python by its full path if the
  script needs one.
- Presses that arrive while one is still running are counted, and up and down
  cancel out, so a burst of taps never plays back late. Put `{steps}` in a
  command and it runs once with the count instead of once per press
  (`... volume up {steps}`).
- Each run is given 15 seconds; failures and the script's last output go to
  `/tmp/crossplay-unlock.log`.
- Try it from a terminal first: `crossplay-unlock tv` shows what is set up,
  `crossplay-unlock tv up 2` runs it now and prints what the script said.

While both lines are set, the helper tells the reader (flag `0x02` in the Mac
state), and the buttons read *- TV* and *+ TV*. The reader learns it when it
connects, so after editing `tv.txt`, close and reopen the Remote app. Take the
lines out and - and + are the Mac's volume again.

## Keyboard layout

HID carries key *positions*, not characters, and the Mac decides what each
position produces. The reader types a **US layout**. On any other layout the
letters land but the symbols do not, so a password with punctuation in it will
not go in. There is nothing the reader can do about this -- nothing comes back
over HID -- so the symptom is a password that silently fails.

## The wire

Two characteristics on one service, and every byte is defined by
`src/apps_local/remote/RemoteVault.h` in the firmware:

| | |
| --- | --- |
| service | `6F1B0A00-9D3C-4F5E-8A77-2B4C1D6E9F01` |
| challenge (notify) | `6F1B0A01-...` -- 58 bytes, reader to Mac |
| response (write) | `6F1B0A02-...` -- 11 bytes plus payload plus 32, Mac to reader |
| now playing (write) | `6F1B0A03-...` -- up to 132 bytes, Mac to reader; `RemoteCore.h` has the layout |
| status (write) | `6F1B0A06-...` -- one board row per write, up to 87 bytes; `RemoteCore.h`, "Status boards" |

`host-tests/remotevault` proves that header against RFC 4231, RFC 7914 and
FIPS 180-4. If this agent and the reader ever disagree, one of them has drifted
from it; the vectors say which.

## What a working run looks like

Four lines on startup and two per press. This is a real unlock, end to end:

```text
[2026-09-23T15:20:22Z] crossplay-unlock running
[2026-09-23T15:20:28Z] found the reader among the peripherals macOS already has
[2026-09-23T15:20:28Z] connected
[2026-09-23T15:20:28Z] listening for challenges
[2026-09-23T15:29:12Z] unlock: locked, sending
[2026-09-23T15:29:18Z] status: screen is awake
```

`found the reader among the peripherals macOS already has` is the line worth
knowing: a CoreBluetooth central really can talk GATT to the peripheral macOS
is already holding for HID, so the scan path below it is a fallback that never
had to run.

The second pair is the unlock itself. `unlock: locked, sending` is this agent
deciding the screen is genuinely locked and releasing the password; `status:
screen is awake` six seconds later is the READER asking what happened, and it
is the only confirmation either side gets that the password was accepted. Six
seconds is the expected gap: about 1.4s waking the display, then the
keystrokes, then the reader's 2.5s follow-up delay.

Now playing adds one line per change, and one when the reader reconnects,
because the reader forgets the song whenever its radio goes down:

```text
[2026-09-23T16:02:40Z] now playing: Harvest Moon by Neil Young (playing)
[2026-09-23T16:05:51Z] now playing: Harvest Moon by Neil Young (paused)
```

A line saying `write to 6F1B0A03-... failed` means the reader refused the
frame, which should only happen if the link was not encrypted; the helper
retries on the next change.

`crossplay-unlock status` answers the same question from this side. Its
`counter` is the high-water mark of challenges that VERIFIED -- it is bumped
only after the MAC checks out -- so a number above zero is proof the pairing
is right. `wrong` is how many wrong answers in a row
it has seen, and says LOCKED OUT once it reaches ten. `mics` is how many input
devices are muted right now.

The microphone button adds a line per press:

```text
[2026-09-28T09:14:02Z] microphones: muted 2 of 2
[2026-09-28T09:31:40Z] microphones: unmuted
```

`cannot mute <device>` means that device offers neither a mute nor a volume a
program may set; it is the one kind this cannot silence.

## If it does not work

| What you see | What it means |
| --- | --- |
| The padlock stays a question mark | The agent is not running, or Bluetooth permission was denied. `tail /tmp/crossplay-unlock.log` |
| "The Mac is connected, but the unlock helper is not running" | HID is up (every other button works) and nothing has subscribed to the challenge characteristic |
| "The Mac did not answer" | The helper is pausing or LOCKED OUT (`crossplay-unlock status`, then `crossplay-unlock unblock`), or holds a different pairing than the reader. The log says which: `locked out; ignoring`, or `a challenge arrived that this reader did not sign` for a mismatch. A helper built before the fix for it kept the secret it started with, so every re-pair needed `launchctl kickstart -k gui/$(id -u)/com.crossplay.unlock` -- and its refusals counted towards the lockout. Current builds read the secret for every request |
| The microphone button never fills | The Mac has not reported every input muted. `crossplay-unlock status` shows how many are; the log names any it `cannot mute` |
| No song on the reader | Nothing has played, paused or changed track since the helper started, or the player is a browser. Press play |
| `Killed: 9` when running `crossplay-unlock` | A new build was copied over the old one in place. `sudo rm /usr/local/bin/crossplay-unlock`, copy it again, then `launchctl kickstart -k gui/$(id -u)/com.crossplay.unlock`. `update.sh` does this itself |
| The log says "replayed" | The two counters are out of step. Re-pairing used to cause it; `pair` now resets this side and the running helper re-reads it before every request, so a build from before those fixes is the likely reason. Delete `~/Library/Application Support/CrossPlayUnlock/ledger.json`, then `launchctl kickstart -k gui/$(id -u)/com.crossplay.unlock` |
| The password is typed but wrong | Non-US keyboard layout, or the password changed since `pair` -- `crossplay-unlock password` fixes the second without disturbing the pairing |
| `status` says `password: missing` after a password reset | The login Keychain was reset with it, which happens when the password is recovered through an Apple ID rather than changed in System Settings. Re-pair |
