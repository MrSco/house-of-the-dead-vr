package com.mrsco.hotdvr;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;

/** Desktop test of UpdateVersion (the updater's version order and release choice; as the GoldenEye VR project's).
 *  From the repo root, with any JDK 11+:
 *
 *    javac -d build_updater_test src/quest/java/com/mrsco/hotdvr/UpdateVersion.java tools/updater_test/UpdateVersionTest.java
 *    java -cp build_updater_test com.mrsco.hotdvr.UpdateVersionTest */
public class UpdateVersionTest {
    static int fails, runs;
    static void eq(Object got, Object want, String what) {
        runs++;
        if (!Objects.equals(got, want)) { fails++; System.out.println("FAIL " + what + ": got " + got + ", want " + want); }
    }
    static UpdateVersion.Release rel(String tag, boolean pre, boolean draft, boolean apk) {
        UpdateVersion.Release r = new UpdateVersion.Release();
        r.tag = tag; r.prerelease = pre; r.draft = draft; r.apkUrl = apk ? "https://x/" + tag + ".apk" : null;
        return r;
    }
    static String tag(UpdateVersion.Release r) { return r == null ? null : r.tag; }

    public static void main(String[] a) {
        // ordering
        String[][] lt = { { "v0.3.0", "v0.4.0" }, { "0.4.9", "0.4.10" }, { "v0.4.0", "0.5" }, { "0.4.1-test.1", "0.4.1" },
                          { "0.4.1-test.1", "0.4.1-test.2" }, { "0.4.1-test.2", "0.4.1-test.10" }, { "0.4.1-rc.1", "0.4.1-test.1" },
                          { "0.4.1-1", "0.4.1-alpha" }, { "0.4.1-test", "0.4.1-test.1" }, { "0.4.0", "0.4.1-test.1" },
                          { "0.9", "0.10" }, { "1.0.0", "1.0.1" } };
        for (String[] p : lt) {
            eq(Integer.signum(UpdateVersion.compare(p[0], p[1])), -1, p[0] + " < " + p[1]);
            eq(Integer.signum(UpdateVersion.compare(p[1], p[0])), 1, p[1] + " > " + p[0]);
        }
        String[][] same = { { "v0.4.0", "0.4.0" }, { "0.4", "0.4.0" }, { "V1.2.3", "1.2.3+build5" }, { " v0.4.0 ", "0.4.0" } };
        for (String[] p : same) eq(UpdateVersion.compare(p[0], p[1]), 0, p[0] + " == " + p[1]);

        // pick
        List<UpdateVersion.Release> rs = Arrays.asList(
            rel("v0.4.2-test.1", true, false, true), rel("v0.4.3", false, true, true), rel("v0.4.1", false, false, true),
            rel("v0.4.0", false, false, true), rel("v0.4.4", false, false, false), null);
        eq(tag(UpdateVersion.pick(rs, "0.4.0", false)), "v0.4.1", "a release: drafts and APK-less ones skipped");
        eq(tag(UpdateVersion.pick(rs, "0.4.0", true)), "v0.4.2-test.1", "test builds on");
        eq(tag(UpdateVersion.pick(rs, "0.4.1", false)), null, "up to date");
        eq(tag(UpdateVersion.pick(rs, "0.4.2-test.1", true)), null, "on the newest test build");
        eq(tag(UpdateVersion.pick(rs, "0.4.2-test.1", false)), null, "a test build installed, test builds off: no older release");
        eq(tag(UpdateVersion.pick(Arrays.asList(rel("v0.4.2", false, false, true)), "0.4.2-test.1", false)), "v0.4.2",
           "the release of the test build installed");
        eq(tag(UpdateVersion.pick(new ArrayList<>(), "0.4.0", true)), null, "no releases");

        // whose APK
        eq(UpdateVersion.apkDevice("house-of-the-dead-vr-0.4.0.apk"), "quest", "the Quest's");
        eq(UpdateVersion.apkDevice("house-of-the-dead-vr-0.4.1-test.1.apk"), "quest", "the Quest's test build");
        eq(UpdateVersion.apkDevice("house-of-the-dead-vr-0.4.1-frame.apk"), "frame", "the Frame's");
        eq(UpdateVersion.apkDevice("house-of-the-dead-vr-0.4.1-test.1-Frame.APK"), "frame", "the Frame's, any case");
        eq(UpdateVersion.apkDevice("house-of-the-dead-vr-0.4.1-android.apk"), "android", "the phones'");
        eq(UpdateVersion.deviceForPackage("com.mrsco.hotdvr"), "quest", "the Quest app");
        eq(UpdateVersion.deviceForPackage("com.mrsco.hotdvr.frame"), "frame", "the Frame app");
        eq(UpdateVersion.deviceForPackage("com.mrsco.hotdvr.android"), "android", "the phone app");
        eq(UpdateVersion.isApkFor("house-of-the-dead-vr-0.4.1.apk", "quest"), true, "the Quest takes its own");
        eq(UpdateVersion.isApkFor("house-of-the-dead-vr-0.4.1-frame.apk", "quest"), false, "the Quest never the Frame's");
        eq(UpdateVersion.isApkFor("house-of-the-dead-vr-0.4.1.apk", "frame"), false, "the Frame never the Quest's");
        eq(UpdateVersion.isApkFor("notes.txt", "quest"), false, "not an APK");
        eq(UpdateVersion.isPreferredApk("house-of-the-dead-vr-0.4.1.apk"), true, "the project's APK");
        eq(UpdateVersion.isPreferredApk("hotdvr.apk"), false, "another APK");

        System.out.println(fails == 0 ? "ok: " + runs + " checks" : fails + " of " + runs + " checks failed");
        if (fails != 0) System.exit(1);
    }
}
