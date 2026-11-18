package com.mrsco.hotdvr;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.security.MessageDigest;
import java.util.Arrays;

/** Installing an update (Updater found it; the SYSTEM tab's UPDATE starts this), a 2D panel in a process of its own
 *  (":update"), as ImportActivity is. As the GoldenEye VR launcher's UpdateChecker:
 *
 *  1. "Install unknown apps" for this app, if it isn't on yet: its settings page, and back here.
 *  2. The APK downloaded into files/updates, its size and GitHub's SHA-256 checked, then that it is this package, no
 *     older than this one and signed with this app's key: Android refuses anything else, and the only way round that
 *     (uninstalling) deletes your ROM set and saves.
 *  3. The system installer, through a PackageInstaller session: it asks you to confirm. A successful update ends the
 *     app (this panel and the game); start it again from the Library. Anything else: try again, or back to the game. */
public final class UpdateActivity extends Activity {
    private static final String TAG = "hotdvr-update";
    private static final String ACTION_STATUS = "com.mrsco.hotdvr.UPDATE_INSTALL_STATUS";
    private static final long MAX_APK = 512L << 20;

    private TextView text;
    private Button primary, back;
    private String tag, name, url, sha256, installed;
    private boolean prerelease;
    private long size;
    private volatile boolean busy, cancelled, askedPermission;
    private volatile File apk;            // verified, ready to install
    private volatile int session = -1;   // the install session waiting on the installer (set by the install thread)
    private long lastSay;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Intent in = getIntent();
        tag = in.getStringExtra("tag");
        prerelease = in.getBooleanExtra("prerelease", false);
        name = in.getStringExtra("name");
        url = in.getStringExtra("url");
        size = in.getLongExtra("size", -1);
        sha256 = in.getStringExtra("sha256");
        installed = Updater.installedVersion(this);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(48, 48, 48, 48);
        box.setBackgroundColor(Color.rgb(12, 19, 29));
        text = new TextView(this);
        text.setTextColor(Color.rgb(238, 243, 246));
        text.setTextSize(20);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        back = new Button(this);
        back.setText("Back to the game");
        back.setOnClickListener(v -> cancelOrBack());
        primary = new Button(this);
        primary.setOnClickListener(v -> go());
        row.addView(back);
        row.addView(primary);
        box.addView(row);
        setContentView(box);

        if (tag == null || url == null) { say("No update to install."); button(null); return; }
        registerStatus();
        new Thread(this::deleteDownloads, "updates-clean").start();
        if (saved == null) go();
    }

    private String title() {
        return "Update The House of the Dead VR\n\nv" + UpdateVersion.normalize(installed) + "  →  v"
               + UpdateVersion.normalize(tag) + (prerelease ? " (a test build)" : "") + "\n\n";
    }
    private void say(String s) { runOnUiThread(() -> text.setText(title() + s)); }
    /** the right-hand button: its label, or none */
    private void button(String label) {
        runOnUiThread(() -> {
            primary.setVisibility(label == null ? View.GONE : View.VISIBLE);
            if (label != null) primary.setText(label);
            primary.setEnabled(!busy);
            back.setText(busy ? "Cancel" : "Back to the game");
        });
    }

    /** the next step: the permission, the download, or the installer */
    private void go() {
        if (busy) return;
        if (!canInstall()) { askPermission(); return; }
        askedPermission = false;
        final File ready = apk;
        if (ready != null && ready.isFile()) { install(ready); return; }
        busy = true;
        cancelled = false;
        button(null);
        new Thread(() -> {
            try {
                File f = download();
                say("Checking the download...");
                verify(f);
                apk = f;
                busy = false;
                install(f);
            } catch (Exception e) {
                busy = false;
                if (cancelled) { say("Update cancelled."); button("Try again"); }
                else {
                    Log.w(TAG, "update failed", e);
                    say("That didn't work: " + Updater.reason(e) + "\n\nTry again, or go back to the game.");
                    button("Try again");
                }
            }
        }, "update").start();
    }

    // ---- 1. the permission ----

    private boolean canInstall() { return Build.VERSION.SDK_INT < 26 || getPackageManager().canRequestPackageInstalls(); }

    private void askPermission() {
        askedPermission = true;
        say("First, let this app install its updates: in the window that opens, switch on \"Allow from this source\" "
            + "(Install unknown apps), then come back here.");
        button("Open the setting");
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            Log.e(TAG, "no settings page for unknown sources", e);
            say("This headset has no \"Install unknown apps\" setting to open. Install the update with SideQuest "
                + "instead (don't uninstall first: that deletes your ROM set and saves).");
            button(null);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        // back from the settings page: on with the update once it is allowed
        if (askedPermission && !busy && canInstall()) go();
    }

    // ---- 2. the download, checked ----

    private File dir() throws IOException {
        File d = getExternalFilesDir("updates");
        if (d == null) throw new IOException("no storage for the download");
        if (!d.isDirectory() && !d.mkdirs()) throw new IOException("could not make " + d);
        return d;
    }
    private void deleteDownloads() {
        File d = getExternalFilesDir("updates");
        File[] fs = d != null ? d.listFiles() : null;
        if (fs != null) for (File f : fs) if (f.isFile() && (apk == null || !f.equals(apk))) f.delete();
    }

    private File download() throws IOException {
        String file = name != null ? name.replaceAll("[^A-Za-z0-9._-]", "_") : "update.apk";
        File part = new File(dir(), file + ".part"), out = new File(dir(), file);
        say("Downloading...");
        HttpURLConnection c = Updater.open(url, "application/octet-stream", installed);
        try {
            int code = c.getResponseCode();
            if (code != 200) throw new IOException("the download failed (HTTP " + code + ")");
            long total = size > 0 ? size : c.getContentLengthLong(), got = 0;
            try (InputStream in = c.getInputStream(); OutputStream o = new FileOutputStream(part)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) {
                    if (cancelled) throw new IOException("cancelled");
                    o.write(buf, 0, n);
                    got += n;
                    if (got > MAX_APK) throw new IOException("the download is too large");
                    long now = System.currentTimeMillis();
                    if (now - lastSay > 250) {
                        lastSay = now;
                        say("Downloading... " + (total > 0 ? (got * 100 / total) + "%  " : "") + (got >> 20) + " MB");
                    }
                }
            }
            if (size > 0 && got != size) throw new IOException("the download is incomplete (" + got + " of " + size + " bytes)");
        } catch (IOException e) {
            part.delete();
            throw e;
        } finally {
            c.disconnect();
        }
        out.delete();
        if (!part.renameTo(out)) { part.delete(); throw new IOException("could not save the download"); }
        return out;
    }

    /** the same bytes GitHub has, this package, a version Android will take, this app's signing key */
    private void verify(File f) throws Exception {
        if (sha256 != null && !sha256(f).equalsIgnoreCase(sha256)) {
            f.delete();
            throw new IOException("the download is damaged (its checksum isn't GitHub's)");
        }
        PackageManager pm = getPackageManager();
        String pkg = getPackageName();
        PackageInfo mine = pm.getPackageInfo(pkg, signingFlags());
        PackageInfo theirs = pm.getPackageArchiveInfo(f.getAbsolutePath(), signingFlags());
        if (theirs == null) { f.delete(); throw new IOException("the download isn't a valid APK"); }
        if (!pkg.equals(theirs.packageName)) { f.delete(); throw new IOException("the download is another app (" + theirs.packageName + ")"); }
        if (versionCode(theirs) < versionCode(mine)) {
            f.delete();
            throw new IOException("that build is older than the one installed (versionCode " + versionCode(theirs) + " < " + versionCode(mine) + ")");
        }
        if (!sameSigner(mine, theirs)) {
            f.delete();
            throw new IOException("v" + UpdateVersion.normalize(tag) + " is signed with another key than this copy, so Android "
                                  + "would refuse it (a test build made on another computer?). Install the release with "
                                  + "SideQuest instead; don't uninstall first: that deletes your ROM set and saves");
        }
    }

    // ---- 3. the installer ----

    private void install(File f) {
        busy = true;
        button(null);
        say("Confirm the update in the window that opens.\n\nThe game closes when it is installed: start it again from "
            + "the Library (Unknown Sources). Your ROM set, settings and saves are kept.");
        // (written now: the next start says "Updated to ..." if it took)
        getSharedPreferences(Updater.PREFS, Context.MODE_PRIVATE).edit()
            .putString(Updater.PREF_PENDING, UpdateVersion.normalize(tag)).commit();
        new Thread(() -> {
            try {
                PackageInstaller pi = getPackageManager().getPackageInstaller();
                PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                params.setAppPackageName(getPackageName());
                params.setSize(f.length());
                int id = pi.createSession(params);
                try (PackageInstaller.Session s = pi.openSession(id)) {
                    try (InputStream in = new FileInputStream(f); OutputStream out = s.openWrite("base.apk", 0, f.length())) {
                        byte[] buf = new byte[1 << 16];
                        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                        s.fsync(out);
                    }
                    Intent cb = new Intent(ACTION_STATUS).setPackage(getPackageName());
                    int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                    session = id;
                    s.commit(PendingIntent.getBroadcast(this, id, cb, flags).getIntentSender());
                    Log.i(TAG, "install session " + id + " committed (" + f.getName() + ")");
                } catch (IOException | RuntimeException e) {
                    session = -1;
                    try { pi.abandonSession(id); } catch (Exception ignored) { }
                    throw e;
                }
            } catch (Exception e) {
                Log.e(TAG, "install", e);
                failed("Could not start the installer: " + Updater.reason(e) + ". Install the update with SideQuest instead "
                       + "(don't uninstall first: that deletes your ROM set and saves).");
            }
        }, "install").start();
    }

    private final BroadcastReceiver status = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent in) {
            int id = in.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1);
            if (id < 0 || id != session) return;
            int st = in.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
            String msg = in.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            Log.i(TAG, "install status " + st + (msg != null ? " (" + msg + ")" : ""));
            if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                @SuppressWarnings("deprecation")
                Intent confirm = in.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm == null) { failed("The installer didn't ask for confirmation."); return; }
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try { startActivity(confirm); } catch (Exception e) { failed("Could not show the installer's confirmation."); }
                return;
            }
            session = -1;
            if (st == PackageInstaller.STATUS_SUCCESS) say("Updated. Start The House of the Dead VR again from the Library.");
            else if (st == PackageInstaller.STATUS_FAILURE_ABORTED) failed("Update cancelled.");
            else failed("The update failed" + (msg != null ? ": " + msg : "."));
        }
    };

    private void registerStatus() {
        IntentFilter f = new IntentFilter(ACTION_STATUS);
        // only the installer's PendingIntent, sent as this app, may reach it
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(status, f, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(status, f);
    }

    private void failed(String why) {
        busy = false;
        getSharedPreferences(Updater.PREFS, Context.MODE_PRIVATE).edit().remove(Updater.PREF_PENDING).commit();
        say(why);
        button("Try again");
    }

    @Override protected void onDestroy() {
        try { unregisterReceiver(status); } catch (Exception ignored) { }
        super.onDestroy();
    }

    @Override public void onBackPressed() { cancelOrBack(); }

    /** Cancel: a download stops; an installer's prompt that never answered (closed from the shell: no result comes)
     *  stops being waited for, its session abandoned; otherwise back to the game */
    private void cancelOrBack() {
        final int id = session;
        if (id >= 0) {
            session = -1;
            try { getPackageManager().getPackageInstaller().abandonSession(id); } catch (Exception e) { Log.w(TAG, "abandon " + id, e); }
            Log.i(TAG, "install session " + id + " abandoned");
            failed("Update cancelled.");
        } else if (busy) cancelled = true;
        else backToVr();
    }

    /** the game again, where it was (its process is still there; this one ends) */
    private void backToVr() {
        Log.i(TAG, "update: back to the game");
        Intent in = new Intent(Intent.ACTION_MAIN)
            .setClassName(getPackageName(), "android.app.NativeActivity")
            .addCategory("org.khronos.openxr.intent.category.IMMERSIVE_HMD")
            .addCategory("com.oculus.intent.category.VR")
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try { startActivity(in); } catch (Exception e) { Log.e(TAG, "back to VR: " + e); }
        finish();
    }

    // ---- helpers ----

    private static String sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) md.update(buf, 0, n);
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest()) sb.append(String.format("%02x", b & 0xff));
        return sb.toString();
    }

    @SuppressWarnings("deprecation")
    private static int signingFlags() {
        return Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
    }

    @SuppressWarnings("deprecation")
    private static Signature[] signers(PackageInfo p) {
        if (Build.VERSION.SDK_INT >= 28) {
            if (p.signingInfo == null) return null;
            return p.signingInfo.hasMultipleSigners() ? p.signingInfo.getApkContentsSigners() : p.signingInfo.getSigningCertificateHistory();
        }
        return p.signatures;
    }

    private static boolean sameSigner(PackageInfo a, PackageInfo b) {
        Signature[] sa = signers(a), sb = signers(b);
        if (sa == null || sb == null || sa.length == 0 || sb.length == 0) return false;
        // one signer: the update's current key must be the installed app's current key
        if (sa.length == 1 || sb.length == 1) {
            for (Signature s : sb) if (s.equals(sa[sa.length - 1])) return true;
            return false;
        }
        Signature[] x = sa.clone(), y = sb.clone();
        Arrays.sort(x, (p, q) -> p.toCharsString().compareTo(q.toCharsString()));
        Arrays.sort(y, (p, q) -> p.toCharsString().compareTo(q.toCharsString()));
        return Arrays.equals(x, y);
    }

    @SuppressWarnings("deprecation")
    private static long versionCode(PackageInfo p) { return Build.VERSION.SDK_INT >= 28 ? p.getLongVersionCode() : p.versionCode; }
}
