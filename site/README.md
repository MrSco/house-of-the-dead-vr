# hotdvr.roccojuliano.com

The website for [The House of the Dead VR](https://github.com/MrSco/house-of-the-dead-vr), served as static assets by the project's Cloudflare Worker (`tools/vr/crash-relay`) on **`https://hotdvr.roccojuliano.com`** alongside the crash relay (`POST /v1/reports`, `GET /v1/health`).

Plain HTML, CSS and a tiny progressive-enhancement script — no framework, no build step.

| Path | What |
| --- | --- |
| `site/public/` | The site exactly as served: `index.html`, `privacy.html`, `404.html`, `styles.css`, `main.js`, images, clips, fonts |
| `site/public/_headers` | Strict CSP, security headers and font caching applied by Cloudflare |
| `site/tools/media.py` | Builds WebP screenshots, icons, OpenGraph card and looping MP4 gameplay clips into `site/public/` |
| `tools/vr/crash-relay/wrangler.jsonc` | Cloudflare Worker config serving `site/public/` at `hotdvr.roccojuliano.com` while routing `/v1/*` and `/report` to the crash relay |

## Preview locally

```bash
cd tools/vr/crash-relay
npm install
npm run dev
```

Open `http://127.0.0.1:8787` to preview the site with Cloudflare's static-asset server, `_headers`, and Worker API routes (`/v1/health`, `/v1/reports`) together.

## Updating screenshots & short gameplay clips

1. Drop new screenshots into `docs/images/play-*.jpg` or `site/media-src/shots/`.
2. Drop higher-resolution Quest MP4 gameplay recordings into `site/media-src/clips/` (matching the clip names `cabinet-courtyard.mp4`, `cabinet-shootout.mp4`, `immersive-reload.mp4`, or add new entries in `CLIPS` in `site/tools/media.py`).
3. Rebuild the web media (requires Python + Pillow + ffmpeg):

```bash
python site/tools/media.py
```

## Deploying to Cloudflare

From `tools/vr/crash-relay` (logged into Wrangler with `npx wrangler login`):

```bash
npm run deploy
```

This updates both the static website at `https://hotdvr.roccojuliano.com/` and the crash relay at `https://hotdvr.roccojuliano.com/v1/reports` in one deploy without disturbing Worker secrets (`GITHUB_TOKEN`).

Fonts: Barlow Semi Condensed, SIL Open Font License (`site/public/fonts/OFL.txt`).
