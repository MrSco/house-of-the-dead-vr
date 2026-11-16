package org.hotdrecomp.vr;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import java.io.File;

/** Online play begins and ends with a restart of the app (src/net/online.c): the game's process starts this, in a
 *  process of its own (":restart"), and ends; this waits until it has, then opens the game again as the launcher does.
 *  The only Java in the app (the Time Crisis II VR project's RestartActivity, the same way). */
public final class RestartActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        final int pid = getIntent().getIntExtra("pid", -1);
        new Thread(() -> {
            File proc = new File("/proc/" + pid);
            for (int i = 0; i < 80 && pid > 0 && proc.exists(); i++) {
                try { Thread.sleep(100); } catch (InterruptedException e) { break; }
            }
            if (pid > 0 && proc.exists()) { Log.w("hotdvr", "restart: the game is still running after 8 s, ending it"); android.os.Process.killProcess(pid); }
            runOnUiThread(() -> {
                Intent in = new Intent(Intent.ACTION_MAIN)
                    .setClassName(getPackageName(), "android.app.NativeActivity")
                    .addCategory("org.khronos.openxr.intent.category.IMMERSIVE_HMD")
                    .addCategory("com.oculus.intent.category.VR")
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try { startActivity(in); } catch (Exception e) { Log.e("hotdvr", "restart: " + e); }
                finish();
                System.exit(0);
            });
        }, "restart").start();
    }
}
