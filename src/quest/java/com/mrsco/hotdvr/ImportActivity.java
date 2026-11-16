package com.mrsco.hotdvr;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Loading your ROM set: the app holds none of the game, so the first time (or from the SYSTEM tab's Change ROM set)
 *  the VR app starts this, in a process of its own (":import"). You choose your MAME ROM set of the 1996 arcade game
 *  on the headset: hotd.zip or hotd.7z, Revision A (MAME's hotd). A .zip is checked here (the polygon ROM is in it,
 *  and the program ROM is Revision A's); a .7z can't be looked into, so only its signature is (the game checks every
 *  ROM when it starts). It is copied into the app's files folder (files/arcade/hotd.zip or hotd.7z), you are offered to
 *  delete the original, and the game starts again. */
public final class ImportActivity extends Activity {
    private static final String TAG = "hotdvr";
    private static final int PICK_FILE = 2;
    private static final long PROGRAM_REV_A = 0x42ADC32EL;   // epr-19696a.15's CRC
    private static final long PROGRAM_ORIGINAL = 0x03DA5623L; // ep19696.15's: the original revision (MAME's hotdo)

    private int pid;
    private TextView text;
    private Button again, back;
    private volatile boolean busy;
    private Uri chosen;
    private String chosenName = "";
    private long chosenSize, done, total, lastSay;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        pid = getIntent().getIntExtra("pid", -1);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(48, 48, 48, 48);
        box.setBackgroundColor(Color.rgb(12, 19, 29));
        text = new TextView(this);
        text.setTextColor(Color.rgb(238, 243, 246));
        text.setTextSize(20);
        // the text scrolls in the space the buttons leave: on the headset's panel a long page pushed the buttons
        // off its bottom
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        again = new Button(this);
        again.setText("Choose a file");
        again.setOnClickListener(v -> pickFile());
        back = new Button(this);
        back.setText("Back to the game");
        back.setOnClickListener(v -> backToVr(false));
        row.addView(back);
        row.addView(again);
        box.addView(row);
        setContentView(box);
        say("Load The House of the Dead (arcade)\n\n"
            + "Choose your MAME ROM set of the 1996 arcade game: hotd.zip or hotd.7z, Revision A (MAME's hotd; "
            + "the original revision, hotdo, is another program). The app keeps its own copy (30 to 40 MB).");
        if (state == null) pickFile();
    }

    private void say(String s) { runOnUiThread(() -> text.setText(s)); }
    private void buttons(boolean on) { runOnUiThread(() -> { again.setEnabled(on); back.setEnabled(on); }); }

    private void pickFile() {
        Intent in = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        in.addCategory(Intent.CATEGORY_OPENABLE);
        in.setType("*/*");
        try {   // (only a starting point)
            in.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                        DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download"));
        } catch (Exception e) { /* none */ }
        try { startActivityForResult(in, PICK_FILE); }
        catch (Exception e) {
            say("This headset has no file picker (" + e + ").\n\nCopy hotd.zip to Android/data/" + getPackageName()
                + "/files/arcade/ over USB instead (adb push).");
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FILE || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri u = data.getData();
        busy = true;
        buttons(false);
        new Thread(() -> {
            String err = null;
            try { load(u); }
            catch (Exception e) { Log.e(TAG, "import", e); err = e.getMessage() != null ? e.getMessage() : e.toString(); }
            busy = false;
            if (err != null) { say("That didn't work: " + err + "\n\nChoose something else, or go back."); buttons(true); }
            else offerDelete();
        }, "import").start();
    }

    /** the chosen file: checked, then copied whole into files/arcade */
    private void load(Uri u) throws Exception {
        chosen = u;
        chosenName = displayName(u);
        chosenSize = 0;
        try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.SIZE }, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) chosenSize = c.getLong(0);
        }
        final String name = chosenName.toUpperCase(Locale.ROOT);
        final boolean sevenZip = name.endsWith(".7Z");
        if (sevenZip) {
            byte[] sig = new byte[6];
            try (InputStream in = open(u)) { if (in.read(sig) != 6) throw new IOException(chosenName + " is empty"); }
            if (sig[0] != '7' || sig[1] != 'z' || (sig[2] & 0xff) != 0xBC || (sig[3] & 0xff) != 0xAF)
                throw new IOException(chosenName + " isn't a 7-Zip archive");
        } else {
            if (!name.endsWith(".ZIP")) throw new IOException(chosenName + " isn't a .zip or a .7z (MAME's hotd set is needed)");
            boolean found = false;
            long program = -1;           // the CRC of the program ROM epr-19696a.15 / ep19696.15 (which revision)
            try (ZipInputStream z = new ZipInputStream(open(u))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    String n = e.getName().toLowerCase(Locale.ROOT);
                    if (n.endsWith("19715.17")) found = true;
                    if (n.endsWith("19696a.15") || n.endsWith("19696.15")) {
                        CRC32 crc = new CRC32();
                        byte[] b = new byte[65536];
                        for (int k; (k = z.read(b)) > 0; ) crc.update(b, 0, k);
                        program = crc.getValue();
                    }
                }
            }
            if (!found) throw new IOException(chosenName + " isn't The House of the Dead's arcade ROM set (no mpr-19715.17 in it)");
            if (program == PROGRAM_ORIGINAL)
                throw new IOException(chosenName + " is the original revision of the game (MAME's hotdo). This app plays Revision A "
                                      + "(MAME's hotd, with epr-19696a.15): choose that set");
            if (program != PROGRAM_REV_A) throw new IOException(chosenName + " has no Revision A program ROM (epr-19696a.15)");
        }
        File dir = new File(getExternalFilesDir(null), "arcade");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot make " + dir);
        final String keep = sevenZip ? "hotd.7z" : "hotd.zip";
        File tmp = new File(dir, keep + ".tmp"), dst = new File(dir, keep);
        File other = new File(dir, sevenZip ? "hotd.zip" : "hotd.7z");   // the game reads the .zip first: one set only
        if (other.exists() && !other.delete()) throw new IOException("cannot remove the old " + other.getName());
        done = 0;
        total = chosenSize;
        try (InputStream in = open(u); OutputStream out = new FileOutputStream(tmp)) {
            byte[] b = new byte[1 << 20];
            for (int n; (n = in.read(b)) > 0; ) { out.write(b, 0, n); progress(n); }
        }
        if (dst.exists() && !dst.delete()) throw new IOException("cannot replace " + dst);
        if (!tmp.renameTo(dst)) throw new IOException("cannot put " + dst + " in place");
    }
    private InputStream open(Uri u) throws IOException {
        InputStream in = getContentResolver().openInputStream(u);
        if (in == null) throw new IOException("cannot open " + chosenName);
        return in;
    }
    private String displayName(Uri u) {
        try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (Exception e) { /* below */ }
        return "that file";
    }
    private void progress(long add) {
        done += add;
        long now = System.currentTimeMillis();
        if (now - lastSay > 250) {
            lastSay = now;
            say("Copying " + chosenName + "\n\n" + (total > 0 ? (done * 100 / total) + "%  " : "") + (done >> 20) + " MB");
        }
    }

    /** The original isn't needed any more (the app keeps its own copy): delete it? */
    private void offerDelete() {
        final String mb = chosenSize > 0 ? " (" + (chosenSize >> 20) + " MB)" : "";
        runOnUiThread(() -> new android.app.AlertDialog.Builder(this)
            .setTitle("The game is loaded")
            .setMessage("Delete " + chosenName + mb + " from the headset? The app doesn't need it any more: it keeps its own copy.")
            .setPositiveButton("Delete it", (dlg, w) -> new Thread(() -> { delete(); backToVr(true); }, "delete").start())
            .setNegativeButton("Keep it", (dlg, w) -> backToVr(true))
            .setCancelable(false)
            .show());
    }
    private void delete() {
        say("Deleting...");
        try {
            if (!DocumentsContract.deleteDocument(getContentResolver(), chosen)) Log.w(TAG, "could not delete " + chosenName);
            else Log.i(TAG, "deleted " + chosenName);
        } catch (Exception e) { Log.w(TAG, "could not delete " + chosenName + ": " + e); }
    }

    @Override public void onBackPressed() { if (!busy) backToVr(false); }

    /** the VR app again, started afresh (the one that sent us here is waiting on its setup page: it ends first),
     *  with the new ROM set when the import worked */
    private void backToVr(boolean imported) {
        Log.i(TAG, imported ? "import done: starting the game" : "import: back to the game");
        runOnUiThread(() -> {
            if (pid > 0) android.os.Process.killProcess(pid);
            try { Thread.sleep(300); } catch (InterruptedException e) { /* go on */ }
            Intent in = new Intent(Intent.ACTION_MAIN)
                .setClassName(getPackageName(), "android.app.NativeActivity")
                .addCategory("org.khronos.openxr.intent.category.IMMERSIVE_HMD")
                .addCategory("com.oculus.intent.category.VR")
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            try { startActivity(in); } catch (Exception e) { Log.e(TAG, "back to VR: " + e); }
            finish();
            System.exit(0);
        });
    }
}
