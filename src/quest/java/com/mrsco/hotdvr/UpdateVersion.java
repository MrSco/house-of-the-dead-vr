package com.mrsco.hotdvr;

import java.util.List;
import java.util.Locale;

/** Release tags and which one to offer (Updater). Plain Java, no Android classes, so it can be tested on a desktop
 *  JVM (tools/updater_test). As the GoldenEye VR project's updater.
 *
 *  Tags look like the ones this repo publishes: "v0.4.0", or for test builds published as GitHub pre-releases,
 *  "v0.4.1-test.1". Ordering is semver's: numbers first, then a version with a suffix sorts before the same version
 *  without one (0.4.1-test.2 < 0.4.1), suffixes compared piece by piece. */
final class UpdateVersion {
    private UpdateVersion() { }

    /** One release as the GitHub API lists it, reduced to what the updater needs. */
    static final class Release {
        String tag;
        boolean prerelease, draft;
        String apkName, apkUrl;
        long apkSize = -1;
        String apkSha256;   // hex, when GitHub reports one ("digest": "sha256:..."), else null
    }

    /** "v0.4.0" -> "0.4.0"; trims whitespace. */
    static String normalize(String tag) {
        if (tag == null) return "";
        String t = tag.trim();
        if (t.startsWith("v") || t.startsWith("V")) t = t.substring(1);
        return t;
    }

    /** Negative if a is older than b, zero if the same, positive if newer. */
    static int compare(String a, String b) {
        String[] sa = splitSuffix(normalize(a)), sb = splitSuffix(normalize(b));
        String[] ca = sa[0].split("\\."), cb = sb[0].split("\\.");
        for (int i = 0, n = Math.max(ca.length, cb.length); i < n; i++) {
            long x = i < ca.length ? leadingNumber(ca[i]) : 0, y = i < cb.length ? leadingNumber(cb[i]) : 0;
            if (x != y) return x < y ? -1 : 1;
        }
        // the same numbers: a release beats any pre-release of it
        boolean pa = !sa[1].isEmpty(), pb = !sb[1].isEmpty();
        if (!pa && !pb) return 0;
        if (!pa) return 1;
        if (!pb) return -1;
        String[] ia = sa[1].split("\\."), ib = sb[1].split("\\.");
        for (int i = 0, m = Math.max(ia.length, ib.length); i < m; i++) {
            if (i >= ia.length) return -1;   // fewer pieces sorts first
            if (i >= ib.length) return 1;
            boolean da = isDigits(ia[i]), db = isDigits(ib[i]);
            int c;
            if (da && db) c = Long.compare(Long.parseLong(ia[i]), Long.parseLong(ib[i]));
            else if (da != db) c = da ? -1 : 1;   // numeric pieces sort before words
            else c = ia[i].compareTo(ib[i]);
            if (c != 0) return c < 0 ? -1 : 1;
        }
        return 0;
    }

    /** The newest release with this device's APK that is newer than the installed version, or null. Drafts are never
     *  offered; pre-releases only when asked for. */
    static Release pick(List<Release> releases, String installed, boolean prereleases) {
        Release best = null;
        for (Release r : releases) {
            if (r == null || r.draft || r.tag == null || r.apkUrl == null) continue;
            if (r.prerelease && !prereleases) continue;
            if (best == null || compare(r.tag, best.tag) > 0) best = r;
        }
        return best != null && compare(best.tag, installed) > 0 ? best : null;
    }

    /** Which build an APK asset is for, from the end of its name: the Steam Frame's "...-frame.apk", the phones' and
     *  tablets' "...-android.apk", the Quest's anything else (house-of-the-dead-vr-0.4.0.apk). */
    static String apkDevice(String name) {
        String n = name == null ? "" : name.toLowerCase(Locale.ROOT);
        if (n.endsWith("-frame.apk")) return "frame";
        if (n.endsWith("-android.apk")) return "android";
        return "quest";
    }

    /** The device a package takes the APKs of (scripts/build_quest.py's flavors: com.mrsco.hotdvr, .frame, .android). */
    static String deviceForPackage(String pkg) {
        if (pkg != null && pkg.endsWith(".frame")) return "frame";
        if (pkg != null && pkg.endsWith(".android")) return "android";
        return "quest";
    }

    /** Whether an asset is an APK for that device: a Frame never installs the Quest's. */
    static boolean isApkFor(String name, String device) {
        return name != null && name.toLowerCase(Locale.ROOT).endsWith(".apk") && apkDevice(name).equals(device);
    }

    /** The project's own APK ("house-of-the-dead-vr-*.apk") over anything else attached to the same release. */
    static boolean isPreferredApk(String name) {
        return name != null && name.toLowerCase(Locale.ROOT).startsWith("house-of-the-dead-vr");
    }

    // "0.4.1-test.1+abc" -> ["0.4.1", "test.1"] (build metadata after '+' ignored)
    private static String[] splitSuffix(String v) {
        int plus = v.indexOf('+');
        if (plus >= 0) v = v.substring(0, plus);
        int dash = v.indexOf('-');
        return dash < 0 ? new String[] { v, "" } : new String[] { v.substring(0, dash), v.substring(dash + 1) };
    }
    private static long leadingNumber(String s) {
        int i = 0;
        while (i < s.length() && Character.isDigit(s.charAt(i))) i++;
        if (i == 0) return 0;
        try { return Long.parseLong(s.substring(0, i)); } catch (NumberFormatException e) { return 0; }
    }
    private static boolean isDigits(String s) {
        if (s.isEmpty()) return false;
        for (int i = 0; i < s.length(); i++) if (!Character.isDigit(s.charAt(i))) return false;
        return true;
    }
}
