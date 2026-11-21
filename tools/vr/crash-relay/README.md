# Crash relay and triage

Crash reports from every House of the Dead VR app go to GitHub issues automatically, deduplicated, with a
deterministic digest attached:

```
app (next start after a crash) ──POST──> https://hotdvr.roccojuliano.com/v1/reports ──> GitHub issue `crash` (new or +1)
  quest_report.c, crash_win.c,             the project's Cloudflare Worker (src/)          in the private repository
  crash_apple.m                                                                                   │
                                                                                                  v
     crash-triage.yml (private repository): symbols (release `crash-symbols`) -> frames as file:line, disassembly,
     source at the report's commit, signature label sig-xxxxxxxx; a later issue of the same crash closed into the first
```

## The apps

Every app has the SYSTEM tab's *Crash reports* switch (on by default; on the flat apps' SYSTEM tab too). A crash
appends to `crash.txt` in the app's folder; the next start turns it, the last 200 lines of the crashed run's
`port.log`, the build, the settings and the device into `crash-reports/<time>.json` and POSTs it in the background.
Sent reports become `<time>.sent.json`, refused ones `<time>.rejected.json`; with no network or a server error they are
tried again at the next start (at most 5 sent per start, 10 kept). `crash-reports/last-result.txt` has the last
outcome. Paths in the app's folder are written `<files>` (Apple: the home folder `~`); no account, serial or other
path is included.

| Platforms | Code | Crash handler | Build id (symbols) | Sending |
|---|---|---|---|---|
| Meta Quest, Steam Frame, Android phones and tablets | `src/quest/quest_report.c` (`platform` from the package: quest, frame, android) | signal handler: pc, lr, frame pointers as `libhotdvr.so+0x…` with the nearest dynamic symbol | GNU build id of `libhotdvr.so` | `HttpURLConnection` over JNI |
| Windows, flat and PC VR | `src/platform/windows/crash_win.c` (windows, pcvr) | unhandled-exception filter, `StackWalk64`: `HOTDVR.exe+0x…` | the PDB's GUID and age | WinHTTP |
| iPhone and iPad, Mac, Apple TV, Vision Pro | `src/platform/apple/crash_apple.m` (ios, mac, tvos, visionos) | signal handler: pc, lr, frame pointers as `<executable>+0x…` from its Mach-O header | the executable's LC_UUID | NSURLSession |

The address is `REPORT_URL` in each file. `env.txt` in the app's folder: `QUEST_CRASH_URL=https://…` (another relay),
`QUEST_CRASH_KEY=…` (sent as `X-Report-Key` when the relay has a `REPORT_KEY`), `QUEST_CRASH_TEST=seconds` or
`FLAT_CRASH_TEST=seconds` (crash on purpose that long after the start, to try the whole path; take it out again).
On Apple the front ends keep the last run's `port.log` as `port.prev.log` (they open a new one first thing), which is
what a report sends.

## The relay (`src/`)

House of the Dead VR's own Cloudflare Worker, `hotd-crash-relay`, on the custom domain `hotdvr.roccojuliano.com`
(`wrangler.jsonc`; the account is the logged-in one, its id is not in this repository). It serves the project's static
website from `site/public/` (`GET /`, `/privacy`, `/img/*`, ...) while running the Worker first on `/v1/*` and `/report`:
`GET /v1/health` answers `{"ok":true,"service":"hotdvr","reports":"/v1/reports","configured":…}`; `POST /v1/reports`
takes a report (`/report` is the same, the address the first Quest builds knew). Later services get their own paths
beside it (`/v1/lobbies`…).

It holds the GitHub token, so the apps hold none. It checks and size-limits each report (128 KB), redacts IP and
e-mail addresses (as GoldenEye VR's relay does), rate-limits (6 a minute per address, 30 overall; at most 20 new crash
issues a day), computes a **signature** and either counts the report on the open `crash` issue with that signature (a
comment for the first 10 occurrences and for every build that issue has not seen, after that only the counter in the
issue body) or opens a new issue labelled `crash` and `platform:<platform>`. The signature is per binary: on Android
the signal and the top 4 function names in `libhotdvr.so` (offsets and build ids left out, so the same crash in
another build matches); on Windows and Apple, whose shipped binaries have no names, the fault, the top app frames'
offsets and the build id (the digest groups those across builds). Hidden markers in the issue body
(`<!-- crash-sig: … -->`, `<!-- crash-occurrences: … -->`, `<!-- crash-builds: … -->`) carry the state; report text
cannot fake them. Not configured yet, or GitHub failing: a 503, and the app keeps the report for its next start.

## Symbols

Every build's symbols are assets of the release `crash-symbols` in the private repository, named by build id:

| Platform | Kept locally by | Asset |
|---|---|---|
| Android (Quest, Frame, phones) | `build_quest.py`: `build_quest/symbols/<id>/libhotdvr.so` | `libhotdvr-<id>.so.gz` |
| Windows | `build_windows.py`: `build_windows/symbols/<id>/HOTDVR.pdb` and `HOTDVR.exe` | `HOTDVR-<id>.pdb.gz`, `HOTDVR-<id>.exe.gz` (llvm-symbolizer reads the PDB through the exe) |
| Apple | on the Mac: the Mac app's dSYM (`cmake/flat_apple.cmake`), Xcode's builds and the TestFlight archives (`$HOTD_ARCHIVES`) | `<executable>-<uuid>.dwarf.gz` (the dSYM's DWARF file; `scripts/apple_symbols.py` collects them) |

`python scripts/quest_tools.py symbols [--apple]` uploads whatever is not there yet (`--apple` first fetches the Mac's,
`HOTD_MAC`); `scripts/release.py` runs it on every release (with `--apple` when it builds or uploads Apple apps). The
command refuses a repository that is not private: the symbols are made from the game. The Apple builds compile with
line tables (`scripts/build_info.py`) so their dSYMs give file:line; dSYMs made before that (0.6.0 and older) give
function names only, and the 0.6.0 Mac app (unstripped) is its own symbol file.

## Triage (`triage/`)

`triage/crash-triage.yml` runs in the private repository, from its default branch, with copies of `triage/digest.py`
and `triage/post.py` in its `.github/triage/` (copy them there after changing them here). On a new `crash` issue, on an
occurrence comment for a new build, or by hand (`gh workflow run crash-triage.yml -f issue=N`):

- `digest.py` downloads the build's symbols, symbolizes the app frames (llvm-symbolizer: inlined calls, file:line;
  return addresses one byte back), disassembles around the faulting instruction (not from a dSYM, which holds no
  code), shows each frame's source at the report's commit (the public repository checked out with its history, or the
  version's tag) and whether that function has changed since (following it to another file), the end of `port.log`,
  and a signature label `sig-xxxxxxxx` (the kind of fault and the top four function names: the same crash on any
  build and any platform running the same code), with the other issues that have it;
- `post.py` comments the digest, labels the issue and, for a new issue whose signature an older open crash issue
  has, closes it as that issue's duplicate and adds its relay signature there, so the relay counts it there from then
  on.

No AI is needed: the issue and its digest are written to be read on their own (Copilot Chat on github.com works from
them). An optional analysis by Claude runs only when the private repository has the secret `ANTHROPIC_API_KEY`; without
it those steps are skipped.

## One-time setup

1. **Labels** (done for the current repository): `crash`, and `platform:quest`, `platform:frame`, `platform:android`,
   `platform:windows`, `platform:pcvr`, `platform:ios`, `platform:mac`, `platform:tvos`, `platform:visionos`
   (`gh label create … --repo MrSco/house-of-the-dead-vr-crashes`).
2. **A GitHub token for the relay**: github.com > Settings > Developer settings > Fine-grained tokens > Generate:
   resource owner MrSco, *Only select repositories*: `house-of-the-dead-vr-crashes`, permissions: *Issues: Read and
   write* (Metadata: read is added by itself), nothing else: the relay only reads, opens, edits, labels and comments on
   issues (it commits no files, so no *Contents* permission). An expiry you will remember to renew.
3. **Deploy the Worker** (done; again after changing `src/`):
   ```
   cd tools/vr/crash-relay
   npm install
   npx wrangler login
   npx wrangler deploy                         # https://hotdvr.roccojuliano.com (a brand-new Worker's first deploy
                                               # may need running twice while Cloudflare creates it)
   npx wrangler secret put GITHUB_TOKEN        # paste the token
   npx wrangler secret put REPORT_KEY          # optional: any random string; the apps must then send it (env.txt)
   ```
   `curl https://hotdvr.roccojuliano.com/` then says `"configured":true`.
4. **Symbols**: `python scripts/release.py` uploads them on every release; between releases,
   `python scripts/quest_tools.py symbols [--apple]` (needs `gh auth login`).
5. **Triage**: the private repository's `.github/workflows/crash-triage.yml` and `.github/triage/` (done; copy them again
   after changing `triage/`). Workflows must be allowed in its Actions settings.

## Testing (no deploy needed)

- `npm test`: the relay against an in-memory GitHub (validation, signatures per platform, new issue / comment / counter,
  platform labels, redaction, merged signatures, key, size, rate and daily limits, the Worker entry point and its
  paths, a report written by the C client).
- The real Worker runtime against a mock GitHub: `node test/mock-github.mjs 18790` and
  `npx wrangler dev --port 8787 --var GITHUB_API:http://127.0.0.1:18790 --var GITHUB_TOKEN:test`, then
  `curl -X POST --data-binary @report.json http://127.0.0.1:8787/v1/reports` (`curl http://127.0.0.1:18790/_issues`
  shows what it filed).
- A forced crash sent to it: Windows: `env.txt` in a test files folder with `QUEST_CRASH_URL=http://127.0.0.1:8787/v1/reports`
  and `FLAT_CRASH_TEST=5`, `HOTDVR.exe --files <folder>`; it crashes, `crash.txt` is written; start it again (without
  the crash line) and `crash-reports/last-result.txt` shows the relay's answer. iOS simulator: the same `env.txt` in the
  app's Documents (`xcrun simctl get_app_container <sim> com.mrsco.hotdvr data`), the relay reached through
  `ssh -R 18787:127.0.0.1:8787 <mac>` (`QUEST_CRASH_URL=http://127.0.0.1:18787/v1/reports`), launched with
  `SIMCTL_CHILD_FLAT_NO_ROM_DIALOG=1 xcrun simctl launch <sim> com.mrsco.hotdvr` (no ROM needed).
- `sh test/client/run.sh <NDK>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include/jni.h` (Linux or WSL, gcc): the
  Android client's collection with stubbed Android headers; writes `test/fixtures/client-report.json` for `npm test`.
  `sh test/client/send.sh <JDK>`: its JNI `HttpURLConnection` sending in a desktop JVM against a local server.
- The digest offline: `node test/make-issue.mjs crash.txt issue.json 5 quest 0.6.0 <commit>`, then
  `python triage/digest.py --issue-json issue.json --others-json others.json --symbols-dir <assets> --code <checkout> --out out`
  (`others.json`: `[]`) and `python triage/post.py --dir out --dry-run`.

## Live check (once the token is set)

From the repository's root, in Git Bash:
```
curl -s https://hotdvr.roccojuliano.com/                      # "configured":true

# 1. a synthetic report (the Quest fixture) straight to the relay: a new issue, then the same again counts on it
node -e "const fs=require('fs');process.stdout.write(JSON.stringify({schema:1,platform:'quest',crash:fs.readFileSync('tools/vr/crash-relay/test/fixtures/crash.txt','utf8'),build_info:{version:'live-check',commit:'none'},device:{model:'live check'},settings:{crash_reports:1},log:'live check\n'}))" > "$TEMP/live.json"
curl -s -X POST --data-binary @"$TEMP/live.json" https://hotdvr.roccojuliano.com/v1/reports   # 201 "created", issue N
curl -s -X POST --data-binary @"$TEMP/live.json" https://hotdvr.roccojuliano.com/v1/reports   # 200 "commented", N

# 2. the Windows app end to end (a test folder; no ROM needed); build it first: python scripts/build_windows.py
mkdir -p "$TEMP/hotd-live" && printf 'FLAT_CRASH_TEST=5\r\n' > "$TEMP/hotd-live/env.txt"
build_windows/HOTDVR.exe --files "$(cygpath -w "$TEMP/hotd-live")"      # crashes after 5 s
rm "$TEMP/hotd-live/env.txt"; build_windows/HOTDVR.exe --files "$(cygpath -w "$TEMP/hotd-live")"   # sends it; close it
cat "$TEMP/hotd-live/crash-reports/last-result.txt"         # 201 {"ok":true,"action":"created","issue":M,...}
python scripts/quest_tools.py symbols                       # (that build's PDB, so the digest can name its frames)
gh workflow run crash-triage.yml -f issue=M --repo MrSco/house-of-the-dead-vr-crashes   # (the digest again, with them)
gh issue view M --repo MrSco/house-of-the-dead-vr-crashes --comments   # the issue and its digest
```
Afterwards delete the test issues (`gh issue delete N --repo MrSco/house-of-the-dead-vr-crashes --yes`, M too), their
`sig-` labels, the dev build's `HOTDVR-<id>.*` assets if unwanted (`gh release delete-asset crash-symbols …`), and
`$TEMP/hotd-live`.

## Costs and risks

- Cloudflare Workers free plan: 100,000 requests a day; the rate limits keep any flood far below that. The digest runs
  on GitHub Actions (private repository minutes: a few tens of seconds per new issue).
- The endpoint is public. Anyone who finds it can post fake reports: they are size- and rate-limited, need the optional
  key, can only create or comment on `crash` issues of the one repository, and cannot fake the hidden markers. Report
  text is shown in code blocks and treated as untrusted. Set `REPORT_KEY` if fake reports become a problem (it is only
  an obstacle: it sits in the apps' env.txt).
- Privacy (PRIVACY.md): a report carries the crash, the end of port.log (the game's and the port's own messages), the
  settings, the device model, system version and processor count, the app build, and the time; the relay sees the
  sender's IP address (used only for the rate limit, not stored) and removes IP and e-mail addresses from the text.
  Reports land in a private repository.
