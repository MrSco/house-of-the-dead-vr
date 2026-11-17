package com.mrsco.hotdvr;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/** Online play begins and ends with a restart of the app (src/net/online.c): the game's process starts this, in a
 *  process of its own (":restart"), and ends; this waits until it has, then opens the game again as the launcher does.
 *  The only Java in the app (the Time Crisis II VR project's RestartActivity, the same way). */
public final class RestartActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        final int pid = getIntent().getIntExtra("pid", -1);
        new Thread(() -> {
            // (the app's own processes, as the system lists them: another process's /proc entry is hidden here on
            // Horizon OS, so it always looked gone, and the game could open again inside the old process, whose
            // OpenXR instance still held the headset: a black screen)
            // (1.5 s: the game's process may stay on after its activity closed; it is ended then)
            for (int i = 0; i < 15 && alive(pid); i++) {
                try { Thread.sleep(100); } catch (InterruptedException e) { break; }
            }
            if (alive(pid)) {
                Log.i("hotdvr", "restart: the game's process is still there, ending it");
                android.os.Process.killProcess(pid);
                for (int i = 0; i < 20 && alive(pid); i++) {
                    try { Thread.sleep(100); } catch (InterruptedException e) { break; }
                }
            }
            Log.i("hotdvr", "restart: the game has closed, opening it again");
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

    /** pid is one of this app's running processes */
    private boolean alive(int pid) {
        if (pid <= 0) return false;
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        java.util.List<ActivityManager.RunningAppProcessInfo> ps = am != null ? am.getRunningAppProcesses() : null;
        if (ps == null) return false;
        for (ActivityManager.RunningAppProcessInfo p : ps) if (p.pid == pid) return true;
        return false;
    }
}
