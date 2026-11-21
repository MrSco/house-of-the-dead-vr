"""Build the website's images and looping gameplay clips into site/public/.

    python site/tools/media.py

Sources live in the repository (assets/branding/, docs/images/) and optional
overrides in site/media-src/ (shots/ and clips/).

Screenshots: each image becomes public/img/shots/<name>.webp (1920 wide max)
and <name>-thumb.webp (800 wide), padded cleanly to 16:9 if needed.

Looping clips: silent H.264 MP4 loops (plays inline everywhere like an animated
GIF at a fraction of the size) plus WebP poster frames. Drop higher-resolution
Quest recordings into site/media-src/clips/ and add or update entries in CLIPS
to replace any clip derived from docs/images/gameplay.gif.
"""
from pathlib import Path
import subprocess

from PIL import Image

SITE = Path(__file__).resolve().parent.parent
REPO = SITE.parent
SRC = SITE / "media-src"
OUT = SITE / "public"


def to_16x9(im):
    """Pad (never crop) to 16:9 on near-black if not already 16:9."""
    w, h = im.size
    if abs(w * 9 - h * 16) <= 16:
        return im
    if w * 9 > h * 16:
        nw, nh = w, round(w * 9 / 16)
    else:
        nw, nh = round(h * 16 / 9), h
    canvas = Image.new("RGB", (nw, nh), (9, 7, 8))
    canvas.paste(im, ((nw - w) // 2, (nh - h) // 2))
    return canvas


def fit_width(im, width):
    if im.width <= width:
        return im
    return im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)


def shots():
    dest = OUT / "img" / "shots"
    dest.mkdir(parents=True, exist_ok=True)

    # Pull Quest screenshots from docs/images/ plus any custom shots in site/media-src/shots/
    sources = {}
    for p in sorted((REPO / "docs" / "images").glob("play-*.jpg")):
        sources[p.stem] = p
    custom_dir = SRC / "shots"
    if custom_dir.is_dir():
        for ext in ("*.jpg", "*.jpeg", "*.png", "*.webp"):
            for p in sorted(custom_dir.glob(ext)):
                sources[p.stem] = p

    for stem, src in sorted(sources.items()):
        im = to_16x9(Image.open(src).convert("RGB"))
        fit_width(im, 1920).save(dest / f"{stem}.webp", quality=82, method=6)
        fit_width(im, 800).save(dest / f"{stem}-thumb.webp", quality=76, method=6)
        print("shot", stem, im.size)

    # Video poster for the click-to-play YouTube embed (crisp 1280x720 Quest frame)
    video_src = custom_dir / "video-cover.jpg" if (custom_dir / "video-cover.jpg").exists() else (
        REPO / "docs" / "images" / "play-immersive-bridge.jpg"
    )
    if video_src.exists():
        im = to_16x9(Image.open(video_src).convert("RGB"))
        fit_width(im, 1280).save(dest / "video-cover.webp", quality=82, method=6)
        print("video-cover", im.size)


# Short 5s-10s looping gameplay clips:
# name -> (source path, start_s, end_s, poster_shot_name_or_seconds)
# If site/media-src/clips/<name>.mp4 exists, it is preferred automatically.
DEFAULT_GIF = REPO / "docs" / "images" / "gameplay.gif"

CLIPS = {
    "cabinet-courtyard": (DEFAULT_GIF, 0.0, 6.0, "play-cabinet"),
    "cabinet-shootout": (DEFAULT_GIF, 8.0, 17.5, "play-cabinet-aim"),
    "immersive-reload": (DEFAULT_GIF, 18.0, 24.9, "play-immersive-reload"),
}


def clips():
    """Generate silent looping H.264 MP4s (smooth loop fade) + WebP posters."""
    dest = OUT / "img" / "shots"
    dest.mkdir(parents=True, exist_ok=True)
    custom_clips = SRC / "clips"

    for name, (default_src, start, end, poster_ref) in CLIPS.items():
        override = custom_clips / f"{name}.mp4"
        src = override if override.exists() else default_src
        if not src.exists():
            print("skip clip (missing source):", name, src)
            continue

        length = max(0.5, end - start)
        # Keep GIF-derived clips compact and smooth without amplifying GIF dithering;
        # HD MP4 captures render at crisp 1280p 30fps.
        is_gif = src.suffix.lower() == ".gif"
        scale = "scale=640:360:flags=lanczos,hqdn3d=1.5:1.5:3:3" if is_gif else "scale=1280:-2:flags=lanczos"
        fps = "fps=12" if is_gif else "fps=30"
        vf = (
            f"{fps},{scale},"
            f"fade=t=in:st=0:d=0.22,"
            f"fade=t=out:st={length - 0.28:.2f}:d=0.28"
        )
        out_mp4 = dest / f"{name}.mp4"
        cmd = [
            "ffmpeg", "-v", "error", "-y",
            "-ss", f"{start:.2f}", "-to", f"{end:.2f}",
            "-i", str(src),
            "-vf", vf,
            "-an",
            "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
            "-crf", "27" if is_gif else "26",
            "-preset", "slow", "-movflags", "+faststart",
            str(out_mp4),
        ]
        subprocess.run(cmd, check=True)

        poster_out = dest / f"{name}-poster.webp"
        if isinstance(poster_ref, str):
            shot_poster = REPO / "docs" / "images" / f"{poster_ref}.jpg"
            im = to_16x9(Image.open(shot_poster).convert("RGB"))
            fit_width(im, 800).save(poster_out, quality=78, method=6)
        else:
            tmp = dest / f"{name}-poster.jpg"
            subprocess.run([
                "ffmpeg", "-v", "error", "-y", "-ss", f"{poster_ref:.2f}",
                "-i", str(src), "-frames:v", "1", str(tmp),
            ], check=True)
            im = to_16x9(Image.open(tmp).convert("RGB"))
            fit_width(im, 800).save(poster_out, quality=78, method=6)
            tmp.unlink()

        size_kb = out_mp4.stat().st_size / 1024
        print(f"clip {name}: {length:.1f}s ({size_kb:.0f} KB)")


def brand():
    branding = REPO / "assets" / "branding"
    img = OUT / "img"
    img.mkdir(parents=True, exist_ok=True)

    # Hero key art (1672x941 native)
    banner_src = branding / "banner.png"
    banner = Image.open(banner_src).convert("RGB")
    banner.save(img / "hero.webp", quality=88, method=6)
    print("hero", banner.size, f"{(img / 'hero.webp').stat().st_size / 1024:.0f} KB")

    # OpenGraph / Twitter social card (1200x630, centered 1.905:1 crop of banner)
    bw, bh = banner.size
    target_ratio = 1200 / 630
    crop_h = round(bw / target_ratio)
    top = max(0, (bh - crop_h) // 2)
    og = banner.crop((0, top, bw, min(bh, top + crop_h))).resize((1200, 630), Image.LANCZOS)
    og.save(img / "og.jpg", quality=88, optimize=True)
    print("og", og.size, f"{(img / 'og.jpg').stat().st_size / 1024:.0f} KB")

    # Square icons and nav mark from app-icon.png
    icon = Image.open(branding / "app-icon.png").convert("RGB")
    icon.resize((64, 64), Image.LANCZOS).save(OUT / "favicon-64.png")
    icon.resize((48, 48), Image.LANCZOS).save(
        OUT / "favicon.ico", sizes=[(16, 16), (32, 32), (48, 48)]
    )
    icon.resize((180, 180), Image.LANCZOS).save(OUT / "apple-touch-icon.png")
    icon.resize((96, 96), Image.LANCZOS).save(img / "mark.webp", quality=88, method=6)
    print("brand icons done")


if __name__ == "__main__":
    shots()
    clips()
    brand()
