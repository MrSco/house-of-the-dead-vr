package com.mrsco.hotdvr;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONArray;
import org.json.JSONObject;

/** Updates from this repo's GitHub releases, for the menu's SYSTEM tab (quest_xr.c calls init, status and command
 *  through JNI; as the GoldenEye VR launcher's UpdateChecker).
 *
 *  Check (once a launch, and on CHECK FOR UPDATES): GET /repos/MrSco/house-of-the-dead-vr/releases, the newest release
 *  (and, with TEST BUILDS on, pre-release) whose tag is newer than this app's versionName and which has this device's
 *  APK attached (UpdateVersion.pick).
 *
 *  Update: UpdateActivity, a 2D panel in a process of its own, downloads the APK, checks it (GitHub's size and SHA-256,
 *  this package, not older, signed with this app's key) and hands it to the system installer; that ends this process.
 *  The next launch notices the new version and the SYSTEM tab says so once.
 *
 *  Nothing here blocks the caller: the network runs on one worker thread, and the menu polls the status. */
public final class Updater {
    private static final String TAG = "hotdvr-update";
    static final String REPO = "MrSco/house-of-the-dead-vr";
    private static final String API = "https://api.github.com/repos/" + REPO + "/releases?per_page=20";
    static final String PREFS = "updater", PREF_TEST_BUILDS = "testBuilds", PREF_PENDING = "pendingVersion";

    // states (quest_menu.c reads the same names)
    static final String IDLE = "idle", CHECKING = "checking", UP_TO_DATE = "uptodate", AVAILABLE = "available",
                        ERROR = "error";

    private static Updater self;
    private final Context app;
    private final SharedPreferences prefs;
    private final String installed, device;
    private final ExecutorService worker = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "updater");
        t.setDaemon(true);
        return t;
    });
    private volatile String state = IDLE, message = "";
    private volatile String notice = "";             // "Updated to v0.4.1.", shown while nothing else is
    private volatile UpdateVersion.Release offer;    // what UPDATE installs

    private Updater(Context c) {
        app = c.getApplicationContext();
        prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        installed = installedVersion(app);
        device = UpdateVersion.deviceForPackage(app.getPackageName());
        // did the last run install an update? say so once
        String pending = prefs.getString(PREF_PENDING, null);
        if (pending != null) {
            if (UpdateVersion.compare(installed, pending) >= 0) notice = "Updated to v" + UpdateVersion.normalize(installed) + ".";
            prefs.edit().remove(PREF_PENDING).apply();
        }
        // an earlier update's download (UpdateActivity keeps it while it runs; a new launch has no use for it)
        worker.execute(() -> {
            java.io.File d = app.getExternalFilesDir("updates");
            java.io.File[] fs = d != null ? d.listFiles() : null;
            if (fs != null) for (java.io.File f : fs) if (f.isFile()) f.delete();
        });
    }

    // ---- what quest_xr.c calls (JNI, static) ----

    /** Once, with the app's activity. */
    public static synchronized void init(Context c) {
        if (self == null) self = new Updater(c);
    }

    /** state \t offered version \t (unused) \t message \t test builds (0/1) \t installed version */
    public static String status() {
        Updater u = self;
        if (u == null) return "";
        UpdateVersion.Release o = u.offer;
        String s = u.state, m = u.message;
        if (m.isEmpty() && !AVAILABLE.equals(s) && !ERROR.equals(s)) m = u.notice;
        return s + "\t" + (o != null ? UpdateVersion.normalize(o.tag) + (o.prerelease ? " (test build)" : "") : "") + "\t-1\t"
               + clean(m) + "\t" + (u.testBuilds() ? 1 : 0) + "\t" + UpdateVersion.normalize(u.installed);
    }

    /** "check", "update", "testbuilds:0" / "testbuilds:1" */
    public static void command(String cmd) {
        Updater u = self;
        if (u == null || cmd == null) return;
        synchronized (u) {
            if (cmd.equals("check")) u.check();
            else if (cmd.equals("update")) u.update();
            else if (cmd.startsWith("testbuilds:")) {
                u.prefs.edit().putBoolean(PREF_TEST_BUILDS, cmd.endsWith("1")).apply();
                u.offer = null;
                u.state = IDLE;
                u.check();
            } else Log.w(TAG, "unknown command " + cmd);
        }
    }

    // ---- check ----

    private boolean testBuilds() { return prefs.getBoolean(PREF_TEST_BUILDS, false); }

    private void check() {
        if (CHECKING.equals(state)) return;
        state = CHECKING;
        message = "";
        final boolean tests = testBuilds();
        worker.execute(() -> {
            try {
                UpdateVersion.Release r = UpdateVersion.pick(parse(get(API)), installed, tests);
                offer = r;
                message = "";
                state = r == null ? UP_TO_DATE : AVAILABLE;
                Log.i(TAG, "check: installed " + installed + ", offer " + (r == null ? "none" : r.tag + " " + r.apkName)
                      + (tests ? " (test builds too)" : ""));
            } catch (Exception e) {
                Log.w(TAG, "check failed", e);
                message = "Could not check for updates: " + reason(e);
                state = ERROR;
            }
        });
    }

    private List<UpdateVersion.Release> parse(String json) throws Exception {
        JSONArray arr = new JSONArray(json);
        List<UpdateVersion.Release> out = new ArrayList<>();
        for (int i = 0; i < arr.length(); i++) {
            JSONObject o = arr.getJSONObject(i);
            UpdateVersion.Release r = new UpdateVersion.Release();
            r.tag = o.optString("tag_name", null);
            r.prerelease = o.optBoolean("prerelease", false);
            r.draft = o.optBoolean("draft", false);
            JSONArray assets = o.optJSONArray("assets");
            for (int j = 0; assets != null && j < assets.length(); j++) {
                JSONObject a = assets.getJSONObject(j);
                String name = a.optString("name", "");
                if (!UpdateVersion.isApkFor(name, device)) continue;             // this device's APK only
                if (r.apkUrl != null && !UpdateVersion.isPreferredApk(name)) continue;   // the project's own wins
                r.apkName = name;
                r.apkUrl = a.optString("browser_download_url", null);
                r.apkSize = a.optLong("size", -1);
                String digest = a.optString("digest", "");
                r.apkSha256 = digest.startsWith("sha256:") ? digest.substring(7).toLowerCase() : null;
                if (UpdateVersion.isPreferredApk(name)) break;
            }
            out.add(r);
        }
        return out;
    }

    // ---- update: the 2D panel does it ----

    private void update() {
        final UpdateVersion.Release r = offer;
        if (r == null) { check(); return; }
        Intent in = new Intent()
            .setClassName(app.getPackageName(), "com.mrsco.hotdvr.UpdateActivity")
            .putExtra("pid", android.os.Process.myPid())
            .putExtra("tag", r.tag).putExtra("prerelease", r.prerelease)
            .putExtra("name", r.apkName).putExtra("url", r.apkUrl)
            .putExtra("size", r.apkSize).putExtra("sha256", r.apkSha256)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            app.startActivity(in);
            Log.i(TAG, "update: UpdateActivity started for " + r.tag);
        } catch (Exception e) {
            Log.e(TAG, "update: could not start UpdateActivity", e);
            message = "Could not open the updater: " + reason(e);
            state = ERROR;
        }
    }

    // ---- helpers (UpdateActivity's too) ----

    static String installedVersion(Context c) {
        try {
            String v = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
            return v != null ? v : "0";
        } catch (PackageManager.NameNotFoundException e) {
            return "0";
        }
    }

    static HttpURLConnection open(String url, String accept, String version) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(10000);
        c.setReadTimeout(20000);
        c.setInstanceFollowRedirects(true);   // release assets redirect to GitHub's file host
        c.setRequestProperty("Accept", accept);
        c.setRequestProperty("User-Agent", "HouseOfTheDeadVR/" + UpdateVersion.normalize(version));
        return c;
    }

    private String get(String url) throws IOException {
        HttpURLConnection c = open(url, "application/vnd.github+json", installed);
        c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
        try {
            int code = c.getResponseCode();
            if (code == 403 || code == 429) throw new IOException("GitHub's rate limit was reached; try again in an hour");
            if (code != 200) throw new IOException("GitHub answered HTTP " + code);
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[1 << 14];
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    if (out.size() > (8 << 20)) throw new IOException("GitHub's answer is too large");
                }
                return new String(out.toByteArray(), StandardCharsets.UTF_8);
            }
        } finally {
            c.disconnect();
        }
    }

    static String reason(Exception e) {
        if (e instanceof java.net.UnknownHostException) return "no internet connection";
        if (e instanceof java.net.SocketTimeoutException) return "the connection timed out";
        String m = e.getMessage();
        return m != null ? m : e.getClass().getSimpleName();
    }

    private static String clean(String s) { return s == null ? "" : s.replace('\t', ' ').replace('\n', ' '); }
}
