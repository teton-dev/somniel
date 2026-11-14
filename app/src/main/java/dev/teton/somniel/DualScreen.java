package dev.teton.somniel;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

import java.io.File;

/**
 * The dual-screen companion's window. When the device has a second display (an AYN Thor's bottom screen) and a
 * companion package is installed (dsmod/dualscreen in the data folder), a Presentation with one SurfaceView is shown
 * on it automatically; the native side (dsmod.cpp) renders the companion into that surface and receives its touches.
 * Without a second display, or without a package, nothing is created and nothing changes.
 */
final class DualScreen {
    private static final String TAG = "somniel-ds";

    static native void nativeSurface(Surface s);
    static native void nativeTouch(int action, float nx, float ny);

    private final Activity act;
    private final android.content.SharedPreferences prefs;
    private final DisplayManager dm;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private Pres pres;
    private boolean started;

    DualScreen(Activity a) {
        act = a;
        dm = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
        prefs = a.getSharedPreferences("sidebar", Context.MODE_PRIVATE);
    }

    /** The user's choice (sidebar). On by default: with a second display and a package, the companion just appears. */
    boolean enabled() { return prefs.getBoolean("dualscreen_enabled", true); }

    void setEnabled(boolean on) {
        prefs.edit().putBoolean("dualscreen_enabled", on).apply();
        if (on) { start(); show(); } else hide();
    }

    /** True when the device has a presentation display besides the main one (an AYN Thor's bottom screen). */
    boolean hasSecondDisplay() {
        for (Display d : dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION))
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY) return true;
        for (Display d : dm.getDisplays())
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY && (d.getFlags() & Display.FLAG_PRESENTATION) != 0) return true;
        return false;
    }

    /** The bundled package (assets/dsmod: the Fire Emblem Engage companion 1.0.0 by igawa6, GPL-3.0; source in
     *  third_party/eden_duo_companions). Bump when the assets change so installed copies are replaced. */
    private static final String BUNDLED = "fee-1.0.0";

    /**
     * At start, before the game boots (the load plan is applied while main is loaded): on a device with a second
     * display, copies the bundled package to the data folder's dsmod/ when it is missing or an older bundled copy. A
     * package the user put there themselves (no .bundled stamp) is left alone. Single-screen devices get nothing, so
     * the game code is not patched there.
     */
    static void installBundled(Activity a) {
        File ext = a.getExternalFilesDir(null);
        if (ext == null) return;
        DisplayManager dm = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
        boolean second = false;
        for (Display d : dm.getDisplays())
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY && (d.getFlags() & Display.FLAG_PRESENTATION) != 0) second = true;
        for (Display d : dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION))
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY) second = true;
        if (!second) return;
        File dir = new File(ext, "dsmod"), stamp = new File(dir, ".bundled");
        boolean present = new File(dir, "dualscreen/manifest.json").isFile();
        String have = null;
        try { if (stamp.isFile()) have = new String(java.nio.file.Files.readAllBytes(stamp.toPath()), "UTF-8").trim(); }
        catch (java.io.IOException ignored) { }
        if (present && (have == null || have.equals(BUNDLED))) return; // the user's own package, or already current
        File tmp = new File(ext, "dsmod.tmp");
        try {
            SaveImporter.deleteTree(tmp);
            copyAssets(a.getAssets(), "dsmod", tmp);
            java.nio.file.Files.write(new File(tmp, ".bundled").toPath(), BUNDLED.getBytes("UTF-8"));
            SaveImporter.deleteTree(dir);
            if (!tmp.renameTo(dir)) throw new java.io.IOException("rename failed");
            Log.i(TAG, "installed the bundled companion package " + BUNDLED);
        } catch (java.io.IOException e) {
            Log.w(TAG, "could not install the bundled companion package: " + e);
            SaveImporter.deleteTree(tmp);
        }
    }

    private static void copyAssets(android.content.res.AssetManager am, String path, File out) throws java.io.IOException {
        String[] kids = am.list(path);
        if (kids == null || kids.length == 0) { // a file
            File p = out.getParentFile();
            if (p != null && !p.isDirectory() && !p.mkdirs()) throw new java.io.IOException("mkdir " + p);
            try (java.io.InputStream in = am.open(path); java.io.OutputStream os = new java.io.FileOutputStream(out)) {
                byte[] buf = new byte[1 << 16];
                for (int r; (r = in.read(buf)) > 0; ) os.write(buf, 0, r);
            }
            return;
        }
        if (!out.isDirectory() && !out.mkdirs()) throw new java.io.IOException("mkdir " + out);
        for (String k : kids) copyAssets(am, path + "/" + k, new File(out, k));
    }

    /** Is a companion package for the game installed (data folder dsmod/dualscreen)? */
    boolean hasPackage() {
        File ext = act.getExternalFilesDir(null);
        return ext != null && new File(ext, "dsmod/dualscreen/manifest.json").isFile();
    }

    void start() {
        if (started) return;
        File ext = act.getExternalFilesDir(null);
        if (ext == null || new File(ext, "dsmod_off").exists()) { Log.i(TAG, "dsmod_off: nothing to show"); return; }
        try {
            nativeSurface(null); // probe: this build has the dual-screen host
        } catch (UnsatisfiedLinkError e) {
            Log.i(TAG, "dual-screen support not built in");
            return;
        }
        started = true;
        dm.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) { show(); }
            @Override public void onDisplayRemoved(int id) { if (pres != null && pres.getDisplay().getDisplayId() == id) hide(); }
            @Override public void onDisplayChanged(int id) { }
        }, ui);
        show();
    }

    void stop() { hide(); }

    /** The panel for a presentation: not the default display, flagged for presentations (the Thor's "Screen-2"). */
    private Display findDisplay() {
        Display best = null;
        for (Display d : dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY && d.getState() != Display.STATE_OFF) best = d;
        }
        if (best == null) {
            for (Display d : dm.getDisplays()) {
                if (d.getDisplayId() == Display.DEFAULT_DISPLAY) continue;
                if ((d.getFlags() & Display.FLAG_PRESENTATION) != 0) { best = d; break; }
            }
        }
        return best;
    }

    void show() {
        if (!started || !enabled() || !hasPackage()) return;
        if (pres != null && pres.isShowing()) return;
        Display d = findDisplay();
        if (d == null) return;
        try {
            pres = new Pres(act, d);
            pres.show();
            Log.i(TAG, "companion shown on display " + d.getDisplayId() + " \"" + d.getName() + "\"");
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot show the companion: " + e);
            pres = null;
        }
    }

    void hide() {
        if (pres != null) {
            try { pres.dismiss(); } catch (RuntimeException ignored) { }
            pres = null;
        }
        try { nativeSurface(null); } catch (UnsatisfiedLinkError ignored) { }
    }

    private static final class Pres extends Presentation {
        Pres(Activity a, Display d) { super(a, d); }

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            SurfaceView sv = new SurfaceView(getContext());
            setContentView(sv);
            sv.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder h) { nativeSurface(h.getSurface()); }
                @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { nativeSurface(h.getSurface()); }
                @Override public void surfaceDestroyed(SurfaceHolder h) { nativeSurface(null); }
            });
            sv.setOnTouchListener(new View.OnTouchListener() {
                @Override public boolean onTouch(View v, MotionEvent e) {
                    int a = e.getActionMasked();
                    if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_MOVE || a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL)
                        nativeTouch(a, e.getX() / Math.max(1, v.getWidth()), e.getY() / Math.max(1, v.getHeight()));
                    return true;
                }
            });
        }
    }
}
