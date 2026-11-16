# Crash relay and triage

Crash reports from the Quest build go to GitHub issues automatically, deduplicated, with an AI analysis attached:

```
headset ──(next start after a crash)──> crash relay (Cloudflare Worker) ──> GitHub issue `crash` (new or +1)
  src/quest/quest_report.c                  src/relay.js                       │
                                                                               v
                       .github/workflows/crash-triage.yml: symbols (release `crash-symbols`) -> frames as file:line
                       -> Claude reads the code -> analysis comment; a sure duplicate is closed into the original
```

- **The app** (`src/quest/quest_report.c`, the VR menu's SYSTEM tab: *Crash reports*, on by default) turns
  `crash.txt` from the crashed run, the last 200 lines of its `port.log`, the build (GNU build id, version, commit),
  the VR settings and the headset's model and OS version into `crash-reports/<time>.json` in the app's folder and
  POSTs it in the background. Paths inside the app's folder are written `<files>`; no account, serial or other path
  is included. Sent reports become `<time>.sent.json`, refused ones `<time>.rejected.json`; with no network they are
  tried again at the next start. `crash-reports/last-result.txt` has the last outcome. Without a relay address
  nothing happens at all (crash.txt stays as before).
- **The relay** (`src/`) holds the GitHub token, so the app holds none. It checks and size-limits each report
  (128 KB), rate-limits (6 a minute per address, 30 overall; at most 20 new crash issues a day), computes a
  **signature** (the signal and the top 4 function names in libhotdvr.so; offsets and build ids left out, so the same
  crash in another build matches) and either counts the report on the open `crash` issue with that signature (a
  comment for the first 10 occurrences and for every build that issue has not seen, after that only the counter in
  the issue body) or opens a new issue labelled `crash`. Hidden markers in the issue body (`<!-- crash-sig: … -->`,
  `<!-- crash-occurrences: … -->`, `<!-- crash-builds: … -->`) carry the state; report text cannot fake them.
- **Symbols**: the frames are offsets into the unstripped `libhotdvr.so` of that build. `port.py` keeps a copy of
  every Quest build's library in `build_quest/symbols/<build id>/` (with line tables for `src/`), and
  `python scripts/quest_tools.py symbols` uploads them as `libhotdvr-<build id>.so.gz` to the release `crash-symbols` of the
  private repository. The library is compiled from your copy of the game, so it only ever goes there (the command
  refuses a repository that is not private).
- **Triage** (`.github/workflows/crash-triage.yml`, on a new `crash` issue and on an occurrence comment for a new
  build): `triage/prep.py` downloads the symbols and symbolizes the frames (llvm-symbolizer: on the runner, or
  installed by the workflow) and lists the other crash issues; `anthropics/claude-code-action` reads that and the code
  and writes an analysis (cause with `file:line`, what the player was doing, suggested fix, confidence) and a
  duplicate verdict to files, with no GitHub access and no shell; `triage/post.py` (copied before the analysis runs)
  posts it and, for a high-confidence duplicate, closes the new issue and adds its signature to the original so the
  relay counts it there from then on.

## One-time setup

1. **The label** (once):
   `gh label create crash --repo MrSco/house-of-the-dead-vr-crashes --color B60205 --description "Automatic crash report"`
2. **A GitHub token for the relay**: github.com > Settings > Developer settings > Fine-grained tokens > Generate:
   resource owner MrSco, *Only select repositories*: `house-of-the-dead-vr-crashes`, permissions: *Issues: Read and
   write* (Metadata: read is added by itself), an expiry you will remember to renew.
3. **Deploy the Worker** (a free Cloudflare account is enough):
   ```
   cd tools/vr/crash-relay
   npm install
   npx wrangler login
   npx wrangler secret put GITHUB_TOKEN        # paste the token
   npx wrangler secret put REPORT_KEY          # optional: any random string; the app must then send it
   npx wrangler deploy                         # prints https://hotd-crash-relay.<subdomain>.workers.dev
   ```
   Check it: `curl https://hotd-crash-relay.<subdomain>.workers.dev/` answers `{"ok":true,...}`.
4. **Point the app at it**: set `REPORT_URL` in `src/quest/quest_report.c` to `https://…workers.dev/report` and
   rebuild (or, for one headset, put `QUEST_CRASH_URL=https://…/report` in its `env.txt`). With a `REPORT_KEY`, also
   `QUEST_CRASH_KEY=<key>` in `env.txt` (it is not compiled in).
5. **Symbols**: after each Quest build you install, `python scripts/quest_tools.py symbols` (needs `gh auth login`). It
   creates the release `crash-symbols` the first time. Reports from builds without uploaded symbols are still
   analysed, from the dynamic symbol names only.
6. **Triage**: the reports and the symbols live in the private `MrSco/house-of-the-dead-vr-crashes`, the code in the
   public `MrSco/house-of-the-dead-vr`; the workflow runs in the private one (where the issues are) and checks the
   public one out to read the code. Add the secret `ANTHROPIC_API_KEY`
   (`gh secret set ANTHROPIC_API_KEY --repo MrSco/house-of-the-dead-vr-crashes`) and keep a copy of
   `.github/workflows/crash-triage.yml` on that repository's default branch (issue workflows only run from there).
   Workflows must be allowed in its Actions settings.
7. **Try it**: `QUEST_CRASH_TEST=20` in the headset's `env.txt` crashes the app on purpose 20 s after it starts; start
   it again and the report goes out (`crash-reports/last-result.txt`, then the issue). Remove the line afterwards.

## Testing (no deploy needed)

- `npm test`: the relay against an in-memory GitHub (validation, signatures, new issue / comment / counter, merged
  signatures, key, size, rate and daily limits, the Worker entry point, a report written by the C client).
- `sh test/client/run.sh <NDK>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include/jni.h` (Linux or WSL, gcc): the
  C client's collection with stubbed Android headers; writes `test/fixtures/client-report.json` for `npm test`.
- `sh test/client/send.sh <JDK>` (Linux or WSL): the client's JNI `HttpURLConnection` sending in a desktop JVM
  against a local server (accepted -> sent, 4xx -> rejected, no answer -> kept).
- `node test/mock-github.mjs 18790` and `npx wrangler dev --var GITHUB_API:http://127.0.0.1:18790 --var GITHUB_TOKEN:x`,
  then `curl -X POST --data-binary @test/fixtures/client-report.json http://127.0.0.1:8787/report`: the real Worker
  runtime (with its rate limiter) against a mock GitHub.
- Triage scripts offline: `node test/make-issue.mjs crash.txt issue.json 5`, then
  `python triage/prep.py --issue-json issue.json --others-json others.json --symbols build_quest/game/libhotdvr.so --out out`
  and `python triage/post.py --dir out --dry-run`.

## Costs and risks

- Cloudflare Workers free plan: 100,000 requests a day; the rate limits keep any flood far below that.
- The analysis uses the Anthropic API on every new crash issue and every occurrence on a new build (not on repeats of
  a known build): typically a few cents to a few tens of cents each. The daily cap on new issues bounds it.
- The endpoint is public. Anyone who finds it can post fake reports: they are size- and rate-limited, need the
  optional key, can only create or comment on `crash` issues of the one repository, and cannot fake the hidden
  markers. Report text is shown in code blocks and treated as untrusted by the analysis, which has no tools that act
  on GitHub. Set `REPORT_KEY` if the URL may leak (it is only an obstacle: it is in env.txt on the headset).
- Privacy: a report carries the crash, the end of port.log (the game's and the port's own messages), the VR settings,
  the headset model and OS build, the app build, and the time; the relay sees the sender's IP address (used only for
  the rate limit, not stored). Reports land in a private repository.
