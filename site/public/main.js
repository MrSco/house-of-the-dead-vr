// Progressive enhancement for hotdvr.roccojuliano.com.
// Everything degrades cleanly without JS: mobile shows YouTube/GitHub links,
// video links go straight to YouTube, screenshots and clips open as files,
// controls stack neatly, and download buttons link to GitHub releases/latest.

// 1. Mobile navigation drawer
const topNav = document.querySelector(".top");
const menuBtn = topNav?.querySelector(".menu-btn");
if (menuBtn) {
  const setMenu = (open) => {
    topNav.classList.toggle("open", open);
    menuBtn.setAttribute("aria-expanded", String(open));
  };
  menuBtn.addEventListener("click", () => setMenu(!topNav.classList.contains("open")));
  topNav.querySelectorAll(".top-links a").forEach((link) => link.addEventListener("click", () => setMenu(false)));
  document.addEventListener("click", (event) => { if (!topNav.contains(event.target)) setMenu(false); });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && topNav.classList.contains("open")) { setMenu(false); menuBtn.focus(); }
  });
}

// 2. Click-to-play privacy-friendly YouTube embed
document.querySelectorAll("[data-youtube]").forEach((link) => {
  link.addEventListener("click", (event) => {
    event.preventDefault();
    const frame = document.createElement("iframe");
    frame.src = `https://www.youtube-nocookie.com/embed/${link.dataset.youtube}?autoplay=1&rel=0`;
    frame.title = link.querySelector(".video-label")?.textContent || "Gameplay video";
    frame.allow = "autoplay; encrypted-media; picture-in-picture; fullscreen";
    frame.referrerPolicy = "strict-origin-when-cross-origin";
    link.replaceChildren(frame);
    link.classList.add("is-playing");
    link.removeAttribute("href");
  }, { once: true });
});

// 3. Controls tabs
document.querySelectorAll("[data-tabs]").forEach((root) => {
  const buttons = [...root.querySelectorAll("[role='tab']")];
  const panels = [...root.querySelectorAll("[role='tabpanel']")];
  const activate = (btn) => {
    const targetId = btn.getAttribute("aria-controls");
    buttons.forEach((b) => {
      const on = b === btn;
      b.classList.toggle("is-active", on);
      b.setAttribute("aria-selected", String(on));
    });
    panels.forEach((p) => { p.hidden = p.id !== targetId; });
  };
  buttons.forEach((btn, idx) => {
    btn.addEventListener("click", () => activate(btn));
    btn.addEventListener("keydown", (event) => {
      if (event.key === "ArrowRight") {
        event.preventDefault();
        const next = buttons[(idx + 1) % buttons.length];
        activate(next); next.focus();
      } else if (event.key === "ArrowLeft") {
        event.preventDefault();
        const prev = buttons[(idx - 1 + buttons.length) % buttons.length];
        activate(prev); prev.focus();
      }
    });
  });
});

// 4. Short gameplay clips: loop smoothly while visible on screen, pause offscreen,
//    and stay paused on poster frame for visitors preferring reduced motion.
const stillMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
const loopVideos = [...document.querySelectorAll(".clips-grid video, .gallery video")];
if (loopVideos.length && "IntersectionObserver" in window) {
  const watcher = new IntersectionObserver((entries) => {
    entries.forEach(({ target, isIntersecting }) => {
      if (isIntersecting && !stillMotion.matches) target.play().catch(() => {});
      else target.pause();
    });
  }, { threshold: 0.3 });
  loopVideos.forEach((clip) => watcher.observe(clip));
}

// 5. Lightbox for both short looping clips and full-resolution screenshots
const box = document.querySelector(".lightbox");
const shots = [...document.querySelectorAll(".clips-grid .shot, .gallery .shot")];
let current = 0;

function captionFor(shot) {
  const figcap = shot.closest("figure")?.querySelector("figcaption");
  if (!figcap) return "";
  const title = figcap.querySelector("strong")?.textContent;
  const desc = figcap.querySelector("span")?.textContent;
  if (title && desc) return `${title} — ${desc}`;
  return figcap.textContent.trim();
}

function show(i) {
  current = (i + shots.length) % shots.length;
  const shot = shots[current];
  const img = box.querySelector("img");
  const clip = box.querySelector("video");
  const isClip = shot.hasAttribute("data-video");
  img.hidden = isClip;
  clip.hidden = !isClip;
  if (isClip) {
    img.removeAttribute("src");
    clip.src = shot.href;
    clip.controls = stillMotion.matches;
    if (!stillMotion.matches) clip.play().catch(() => {});
  } else {
    clip.pause();
    clip.removeAttribute("src");
    img.src = shot.href;
    img.alt = shot.querySelector("img")?.alt || "";
  }
  box.querySelector("figcaption").textContent = captionFor(shot);
}

if (box && typeof box.showModal === "function" && shots.length) {
  shots.forEach((shot, i) => {
    shot.addEventListener("click", (event) => {
      event.preventDefault();
      show(i);
      box.showModal();
    });
  });
  box.querySelector(".lb-close").addEventListener("click", () => box.close());
  box.querySelector(".lb-prev").addEventListener("click", () => show(current - 1));
  box.querySelector(".lb-next").addEventListener("click", () => show(current + 1));
  box.addEventListener("click", (event) => {
    if (event.target === box || event.target.tagName === "FIGURE") box.close();
  });
  box.addEventListener("keydown", (event) => {
    if (event.key === "ArrowLeft") show(current - 1);
    if (event.key === "ArrowRight") show(current + 1);
  });
  box.addEventListener("close", () => {
    box.querySelector("img").removeAttribute("src");
    const clip = box.querySelector("video");
    clip.pause();
    clip.removeAttribute("src");
  });
}

// 6. Auto-populate latest release tag and direct asset download links from GitHub
fetch("https://api.github.com/repos/MrSco/house-of-the-dead-vr/releases/latest", {
  headers: { Accept: "application/vnd.github+json" },
})
  .then((res) => (res.ok ? res.json() : null))
  .then((release) => {
    if (!release) return;
    const assets = release.assets || [];
    const questApk = assets.find((a) => /^house-of-the-dead-vr-[\d.]+\.apk$/i.test(a.name));
    document.querySelectorAll("[data-version]").forEach((el) => { el.textContent = release.tag_name; });
    if (questApk) {
      document.querySelectorAll("[data-apk]").forEach((el) => { el.href = questApk.browser_download_url; });
    }
    document.querySelectorAll("[data-asset]").forEach((el) => {
      const file = assets.find((a) => a.name.endsWith(el.dataset.asset));
      if (file) el.href = file.browser_download_url;
    });
  })
  .catch(() => {});
