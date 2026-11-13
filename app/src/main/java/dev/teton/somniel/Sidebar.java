package dev.teton.somniel;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

/**
 * The in-game sidebar: a panel that slides in from the left over the SDL surface (plain Android views, so it needs no
 * extra libraries). Opened by Android's Back (SomnielActivity catches it before SDL, so the game never sees it).
 *
 * Working today: Restart Game, Quit Game, the FPS and Frametime monitors. Visual only so far: the frame-rate
 * drop-down and the render-scale slider (remembered, nothing reads them yet). The Mods page switches mod folders on and off.
 */
final class Sidebar {
    private static final int[] FPS_CHOICES = {30, 40, 60};

    /** Graphics options: native key, preference name, row title, choice names (index = value; 0 = as the game renders). */
    private static final Object[][] GFX = {
        {4, "gfx_profile", "Console mode", new String[] {"Auto", "Handheld (after restart)", "Docked (after restart)"}},
        {7, "gfx_aspect", "Aspect ratio", new String[] {"Stretch to fill", "16:9"}},
        {0, "gfx_shadows", "Shadows", new String[] {"High", "Medium", "Low", "Off"}},
        {6, "gfx_fxaa", "Anti-aliasing", new String[] {"On", "Off"}},
        {2, "gfx_texbias", "Texture sharpness", new String[] {"Normal", "Softer (after restart)", "Soft (after restart)"}},
        {5, "gfx_texsize", "Texture size", new String[] {"Full", "Half (after restart)", "Quarter (after restart)"}},
        {3, "gfx_shaders", "Shader precision", new String[] {"Accurate", "Fast (after restart)", "Fastest, experimental: some text breaks (after restart)"}},
    };

    int graphics(int i) {
        String[] names = (String[]) GFX[i][3];
        int v = prefs.getInt((String) GFX[i][1], 0);
        return v >= 0 && v < names.length ? v : 0;
    }

    /** Sends every saved graphics option to the native side (at start; Console mode is also sent from the constructor,
     *  before anything asks the native side for the profile). */
    void pushGraphics() {
        for (int i = 0; i < GFX.length; i++) SomnielActivity.nativeSetGraphics((Integer) GFX[i][0], graphics(i));
    }
    static final int SCALE_MIN = 50, SCALE_MAX = 125, SCALE_STEP = 5; // percent of the game's render size

    private final SomnielActivity act;
    private final SharedPreferences prefs;
    private final float dp;

    private final FrameLayout root;
    private final View scrim;
    private final LinearLayout panel;
    private final LinearLayout mainPage, modsPage;
    private LinearLayout driversPage, touchPage, graphicsPage;
    private android.widget.RadioGroup driverGroup;
    private TextView driverStatus;
    private View firstFocus, modsFocus, driversFocus, touchFocus, graphicsFocus;
    private boolean open, modsShown, driversShown, touchShown, graphicsShown;
    private ScrollView sv;

    Sidebar(SomnielActivity act) {
        this.act = act;
        this.prefs = act.getSharedPreferences("sidebar", Context.MODE_PRIVATE);
        this.dp = act.getResources().getDisplayMetrics().density;
        // the console profile is latched natively the first time anything asks for it (the default frame rate below)
        SomnielActivity.nativeSetGraphics(4, graphics(0));

        root = new FrameLayout(act);
        root.setVisibility(View.GONE);
        scrim = new View(act);
        scrim.setBackgroundColor(0x99000000);
        scrim.setOnClickListener(v -> close());
        root.addView(scrim, new FrameLayout.LayoutParams(-1, -1));

        panel = new LinearLayout(act);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xF2141824);
        panel.setClickable(true); // swallow touches so they never reach the scrim
        root.addView(panel, new FrameLayout.LayoutParams(px(340), -1, Gravity.START));

        TextView title = text("Somniel", 22, true);
        {   /* the version in small, lighter text after the name */
            String v = "  v" + BuildVersion.name(act);
            android.text.SpannableString ss = new android.text.SpannableString("Somniel" + v);
            int a = "Somniel".length(), b = ss.length();
            ss.setSpan(new android.text.style.RelativeSizeSpan(0.55f), a, b, 0);
            ss.setSpan(new android.text.style.ForegroundColorSpan(0xFF9AA3B8), a, b, 0);
            ss.setSpan(new android.text.style.StyleSpan(Typeface.NORMAL), a, b, 0);
            title.setText(ss);
        }
        title.setPadding(px(20), px(10), px(20), px(4));
        panel.addView(title);

        mainPage = new LinearLayout(act);
        mainPage.setOrientation(LinearLayout.VERTICAL);
        modsPage = new LinearLayout(act);
        modsPage.setOrientation(LinearLayout.VERTICAL);
        modsPage.setVisibility(View.GONE);
        sv = new ScrollView(act);
        LinearLayout pages = new LinearLayout(act);
        pages.setOrientation(LinearLayout.VERTICAL);
        pages.addView(mainPage);
        pages.addView(modsPage);
        driversPage = new LinearLayout(act);
        driversPage.setOrientation(LinearLayout.VERTICAL);
        driversPage.setVisibility(View.GONE);
        pages.addView(driversPage);
        touchPage = new LinearLayout(act);
        touchPage.setOrientation(LinearLayout.VERTICAL);
        touchPage.setVisibility(View.GONE);
        pages.addView(touchPage);
        graphicsPage = new LinearLayout(act);
        graphicsPage.setOrientation(LinearLayout.VERTICAL);
        graphicsPage.setVisibility(View.GONE);
        pages.addView(graphicsPage);
        sv.addView(pages);
        panel.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));

        buildMain();
        buildMods();
        buildDrivers();
        buildTouch();
        buildGraphics();
    }

    View view() { return root; }
    boolean isOpen() { return open; }
    void toggle() { if (open) close(); else open(); }

    /** The monitors' saved state; also pushed to the native overlay at start and on every change. */
    /** 0 off, 1 FPS, 2 FPS + frame times + graph, 3 adds CPU and GPU temperatures. Level 2 until the user changes it. */
    int monitorLevel() { return Math.max(0, Math.min(4, prefs.getInt("monitor_level", 2))); }
    /** The saved frame rate (30, 40 or 60; until the user picks one, the device default: 30 on the handheld profile used
     *  for Mali GPUs, else 60) and render scale (%). */
    int frameRate() {
        int def = SomnielActivity.nativeDefaultFrameRate();
        int f = prefs.getInt("fps_target", def);
        return f == 30 || f == 40 || f == 60 ? f : def;
    }
    int renderScale() {
        int p = prefs.getInt("render_scale", 100);
        return Math.max(SCALE_MIN, Math.min(SCALE_MAX, p / SCALE_STEP * SCALE_STEP));
    }

    /** On-screen controls: on or always off; when on, whether a connected controller (built-in ones too) hides them. */
    boolean touchEnabled() { return prefs.getBoolean("touch_enabled", true); }
    boolean touchHideWithPad() { return prefs.getBoolean("touch_hide_with_pad", true); }
    /** 0 D-pad and left stick, 1 D-pad only, 2 left stick only. */
    int touchMoveMode() { return Math.max(0, Math.min(2, prefs.getInt("touch_move", 0))); }
    boolean touchFade() { return prefs.getBoolean("touch_fade", true); }

    void open() {
        if (open) return;
        open = true;
        showMods(false);
        showDrivers(false);
        showTouch(false);
        showGraphics(false);
        SomnielActivity.nativeSidebarOpen(true);
        root.setVisibility(View.VISIBLE);
        panel.setTranslationX(-px(340));
        panel.animate().translationX(0).setDuration(180).setListener(null).start();
        scrim.setAlpha(0f);
        scrim.animate().alpha(1f).setDuration(180).start();
        firstFocus.requestFocus();
    }

    void close() {
        if (!open) return;
        if (modsShown && pendingModChanges() > 0) { leaveMods(this::closeNow); return; }
        closeNow();
    }

    private void closeNow() {
        if (!open) return;
        open = false;
        SomnielActivity.nativeSidebarOpen(false);
        scrim.animate().alpha(0f).setDuration(150).start();
        panel.animate().translationX(-px(340)).setDuration(150).setListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator a) {
                if (!open) root.setVisibility(View.GONE);
            }
        }).start();
        act.sidebarClosed();
    }

    /** Back or B while a sub-page is showing returns to the main page first. Returns true if it handled the press. */
    boolean back() {
        if (!open) return false;
        if (modsShown) { leaveMods(() -> showMods(false)); return true; }
        if (driversShown) { showDrivers(false); return true; }
        if (touchShown) { showTouch(false); return true; }
        if (graphicsShown) { showGraphics(false); return true; }
        close();
        return true;
    }

    // ---- main page ----

    private void buildMain() {
        Button restart = row("Restart Game", v -> confirm("Restart the game?",
                "The game restarts at the title screen. Unsaved progress is lost.", "Restart", act::restartGame));
        firstFocus = restart;
        mainPage.addView(restart);

        // performance monitor: one slider, Off / 1 / 2 / 3 (like the Steam Deck's), applied at once
        TextView monLabel = label("");
        mainPage.addView(monLabel);
        SeekBar mon = new SeekBar(act);
        mon.setMax(4);
        mon.setKeyProgressIncrement(1);
        mon.setProgress(monitorLevel());
        monLabel.setText(monitorText(monitorLevel()));
        mon.setPadding(px(28), px(8), px(28), px(8));
        mon.setBackground(focusBg());
        mon.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                monLabel.setText(monitorText(p));
                prefs.edit().putInt("monitor_level", p).apply();
                pushMonitors();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        mainPage.addView(mon, new LinearLayout.LayoutParams(-1, px(40)));
        TextView monNote = text("1: FPS   2: + frame times, graph   3: + temperatures   4: + CPU/GPU clocks", 12, false);
        monNote.setTextColor(0xFFD2D8E8);
        monNote.setPadding(px(20), 0, px(20), px(10));
        mainPage.addView(monNote);

        // frame-rate drop-down (visual: nothing reads it yet; 60 is the default until performance detection exists)
        // A row that opens a PopupMenu: a real drop-down, and unlike a Spinner it cannot clip its own text.
        final int[] fpsSel = {frameRate()};
        Button fps = row("", null);
        fps.setText("Frame rate: " + fpsSel[0] + " FPS  ▾");
        fps.setOnClickListener(v -> {
            android.widget.PopupMenu m = new android.widget.PopupMenu(act, fps);
            for (int i = 0; i < FPS_CHOICES.length; i++) m.getMenu().add(0, FPS_CHOICES[i], i, FPS_CHOICES[i] + " FPS");
            m.setOnMenuItemClickListener(it -> {
                fpsSel[0] = it.getItemId();
                prefs.edit().putInt("fps_target", fpsSel[0]).apply();
                SomnielActivity.nativeSetFrameRate(fpsSel[0]); // takes effect at once
                fps.setText("Frame rate: " + fpsSel[0] + " FPS  ▾");
                return true;
            });
            m.show();
        });
        mainPage.addView(fps);

        mainPage.addView(row("On-screen controls...", v -> showTouch(true)));

        // render scale: 50% to 125% in 5% steps (visual: nothing reads it yet)
        final int steps = (SCALE_MAX - SCALE_MIN) / SCALE_STEP;
        TextView scaleLabel = label("");
        mainPage.addView(scaleLabel);
        SeekBar scale = new SeekBar(act);
        scale.setMax(steps);
        scale.setKeyProgressIncrement(1);
        int pct = Math.max(SCALE_MIN, Math.min(SCALE_MAX, prefs.getInt("render_scale", 100)));
        scale.setProgress((pct - SCALE_MIN) / SCALE_STEP);
        scaleLabel.setText(scaleText(pct));
        scale.setPadding(px(28), px(8), px(28), px(8));
        scale.setBackground(focusBg());
        scale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                int v = SCALE_MIN + p * SCALE_STEP;
                scaleLabel.setText(scaleText(v));
                prefs.edit().putInt("render_scale", v).apply();
                SomnielActivity.nativeSetRenderScale(v); // re-applied by the game within a frame
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        mainPage.addView(scale, new LinearLayout.LayoutParams(-1, px(40)));

        mainPage.addView(row("Graphics...", v -> showGraphics(true)));

        if (act.telemetry() != null && act.telemetry().available()) {
            Telemetry tel = act.telemetry();
            mainPage.addView(check("Share anonymous performance & crash data", tel.enabled(), on -> tel.setConsent(on)));
        }
        mainPage.addView(row("Mods...", v -> showMods(true)));
        mainPage.addView(row("Drivers...", v -> showDrivers(true)));
        mainPage.addView(row("Quit Game", v -> confirm("Quit the game?",
                "Unsaved progress is lost.", "Quit", act::quitGame)));
    }

    /** "Render scale: 100%  (1728×972)" with the game's default in a smaller, lighter second line right under it. */
    private CharSequence scaleText(int v) {
        String l1 = "Render scale: " + v + "%  (" + Math.round(1728 * v / 100f) + "×" + Math.round(972 * v / 100f) + ")";
        String l2 = "The game's default is 1728×972";
        android.text.SpannableStringBuilder sb = new android.text.SpannableStringBuilder(l1 + "\n" + l2);
        int s = l1.length() + 1;
        sb.setSpan(new android.text.style.AbsoluteSizeSpan(11, true), s, sb.length(), 0);
        sb.setSpan(new android.text.style.ForegroundColorSpan(0xFFD2D8E8), s, sb.length(), 0);
        return sb;
    }

    private void pushMonitors() {
        SomnielActivity.nativeSetMonitorLevel(monitorLevel());
    }

    private static String monitorText(int level) {
        return "Performance monitor: " + (level == 0 ? "Off" : "Level " + level);
    }

    // ---- mods page ----
    // Cobalt reads the folders in <external files>/sd/engage/mods/<ModName>/ at game start. A mod is switched off by
    // moving its folder to the sibling mods_disabled/ (a rename: instant, whatever the size). Checkbox changes are only
    // staged; Apply moves the folders and asks for a restart, because Cobalt reads the mods once, at boot.

    private LinearLayout modsList;
    private TextView modsStatus, cobaltStatus;
    private Button cobaltDownloadBtn;
    private Button modsApply;
    private final java.util.TreeMap<String, Boolean> modsStaged = new java.util.TreeMap<>(); // name -> wanted on
    private final java.util.Map<String, Boolean> modsOnDisk = new java.util.HashMap<>();     // name -> is on
    private boolean modsNeedRestart; // changes were applied but the running game has not been restarted yet

    private java.io.File modsDir(boolean enabled) {
        java.io.File ext = act.getExternalFilesDir(null);
        return ext == null ? null : new java.io.File(ext, enabled ? "sd/engage/mods" : "sd/engage/mods_disabled");
    }

    private void buildMods() {
        Button back = row("<  Mods", v -> leaveMods(() -> showMods(false)));
        back.setTypeface(Typeface.DEFAULT_BOLD);
        modsFocus = back;
        modsPage.addView(back);

        // Cobalt (the mod loader) is fetched by the user: download the latest release or import a release.zip
        cobaltStatus = text("", 14, false);
        cobaltStatus.setTextColor(0xFFD2D8E8);
        cobaltStatus.setPadding(px(20), px(12), px(20), px(4));
        modsPage.addView(cobaltStatus);
        cobaltDownloadBtn = row("Download latest Cobalt", v -> cobaltDownload(false));
        cobaltDownloadBtn.setTypeface(Typeface.DEFAULT_BOLD);
        modsPage.addView(cobaltDownloadBtn);
        modsPage.addView(row("Import Cobalt zip...", v -> leaveMods(act::pickCobaltZip)));

        modsStatus = text("", 14, false);
        modsStatus.setTextColor(0xFFB0B8CC);
        modsStatus.setPadding(px(20), px(14), px(20), px(8));
        modsPage.addView(modsStatus);

        modsList = new LinearLayout(act);
        modsList.setOrientation(LinearLayout.VERTICAL);
        modsPage.addView(modsList);

        modsApply = row("Apply changes", v -> applyMods(null));
        modsApply.setTypeface(Typeface.DEFAULT_BOLD);
        modsPage.addView(modsApply);
        modsPage.addView(row("Import...", v -> leaveMods(act::pickModArchive)));
        refreshMods();
    }

    private void refreshCobalt() {
        String v = CobaltInstaller.version(act);
        cobaltStatus.setText(v == null ? "Cobalt (the mod loader, by Raytwo) is not installed. Mods need it."
                : "Cobalt: " + (v.equals("installed") ? "installed" : v + " installed"));
        cobaltDownloadBtn.setText(v == null ? "Download latest Cobalt" : "Update Cobalt (download latest)");
    }

    private boolean cobaltBusy;

    /** Opens the sidebar on the Mods page (the boot prompt uses it so the download's progress is visible). */
    void openMods() {
        open();
        showMods(true);
    }

    /** Downloads and installs the newest Cobalt release, then offers the restart. */
    void cobaltDownload(final boolean showPage) {
        if (cobaltBusy) return;
        if (showPage) openMods();
        runCobalt(p -> CobaltInstaller.downloadLatest(act, p));
    }

    /** Installs Cobalt from a release.zip chosen in the file picker. */
    void importCobalt(final android.net.Uri uri) {
        if (cobaltBusy) return;
        openMods();
        runCobalt(p -> {
            p.say("Installing Cobalt from " + displayName(uri) + "...");
            try (java.io.InputStream in = act.getContentResolver().openInputStream(uri)) {
                if (in == null) throw new java.io.IOException("cannot open the file");
                CobaltInstaller.installZip(act, in, "imported");
            }
            return "imported";
        });
    }

    private interface CobaltJob { String run(CobaltInstaller.Progress p) throws Exception; }

    private void runCobalt(final CobaltJob job) {
        cobaltBusy = true;
        cobaltDownloadBtn.setEnabled(false);
        new Thread(() -> {
            String err = null, tag = null;
            try {
                tag = job.run(m -> act.runOnUiThread(() -> cobaltStatus.setText(m)));
            } catch (Throwable e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String error = err, done = tag;
            act.runOnUiThread(() -> {
                cobaltBusy = false;
                cobaltDownloadBtn.setEnabled(true);
                refreshCobalt();
                if (error != null) {
                    Toast.makeText(act, "Cobalt: " + error, Toast.LENGTH_LONG).show();
                    return;
                }
                prefs.edit().putBoolean("cobalt_declined", false).apply();
                confirmRestartForMods("Cobalt " + (done.equals("imported") ? "" : done + " ") + "is installed. It is loaded "
                        + "when the game starts. Restart now? Unsaved progress is lost.");
            });
        }, "cobalt-install").start();
    }

    /** Rebuilds the list from the two folders on disk and clears the staged changes. */
    private void refreshMods() {
        refreshCobalt();
        modsStaged.clear();
        modsOnDisk.clear();
        for (int pass = 0; pass < 2; pass++) {
            java.io.File[] dirs = modsDir(pass == 0) == null ? null : modsDir(pass == 0).listFiles();
            if (dirs != null) for (java.io.File d : dirs)
                if (d.isDirectory() && !d.getName().startsWith(".")) modsOnDisk.put(d.getName(), pass == 0);
        }
        modsStaged.putAll(modsOnDisk);
        modsList.removeAllViews();
        for (java.util.Map.Entry<String, Boolean> e : new java.util.TreeMap<>(modsOnDisk).entrySet()) {
            final String name = e.getKey();
            modsList.addView(check(name, e.getValue(), on -> {
                modsStaged.put(name, on);
                updateModsStatus();
            }));
        }
        updateModsStatus();
    }

    private int pendingModChanges() {
        int n = 0;
        for (java.util.Map.Entry<String, Boolean> e : modsStaged.entrySet())
            if (!e.getValue().equals(modsOnDisk.get(e.getKey()))) n++;
        return n;
    }

    private void updateModsStatus() {
        int pending = pendingModChanges();
        String head = modsOnDisk.isEmpty()
                ? "No mods installed.\nImport a .zip or .7z archive; installed mods are listed here with a checkbox to turn each one on or off."
                : "Tick the mods to use, then press Apply. Mods are read when the game starts, so a restart is needed.";
        if (modsNeedRestart) head += "\n\nMod changes are applied. Restart the game for them to take effect.";
        modsStatus.setText(head);
        modsApply.setEnabled(pending > 0); // always shown; greyed out until a checkbox changed
        modsApply.setAlpha(pending > 0 ? 1f : 0.4f);
        modsApply.setText(pending > 0 ? "Apply changes (" + pending + ")" : "Apply changes");
    }

    /** Moves the folders to match the staged choices; asks for a restart when something changed. */
    private void applyMods(Runnable then) {
        int moved = 0, failed = 0;
        for (java.util.Map.Entry<String, Boolean> e : modsStaged.entrySet()) {
            boolean want = e.getValue();
            if (want == modsOnDisk.get(e.getKey())) continue;
            java.io.File from = new java.io.File(modsDir(!want), e.getKey());
            java.io.File toDir = modsDir(want);
            if (toDir == null || (!toDir.isDirectory() && !toDir.mkdirs())) { failed++; continue; }
            if (from.renameTo(new java.io.File(toDir, e.getKey()))) moved++; else failed++;
        }
        refreshMods();
        if (moved > 0) modsNeedRestart = true;
        updateModsStatus();
        if (failed > 0) Toast.makeText(act, failed + " mod(s) could not be changed.", Toast.LENGTH_LONG).show();
        if (moved > 0) confirmRestartForMods("Mods are loaded when the game starts. Restart now? Unsaved progress is lost.");
        if (then != null) then.run();
    }

    private boolean importing;

    /** "Restart to apply?" after a mod change. IMPORT MORE (left) opens the picker again so several mods can be added first. */
    private void confirmRestartForMods(String msg) {
        new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Restart to apply?").setMessage(msg)
                .setNeutralButton("Import more", (d, w) -> act.pickModArchive())
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Restart", (d, w) -> act.restartGame())
                .show();
    }

    /** Installs a .zip or .7z mod archive chosen in the file picker (ModImporter), then offers the restart. */
    void importMod(final android.net.Uri uri) {
        if (importing) return;
        importing = true;
        final String shown = displayName(uri);
        modsStatus.setText("Importing " + shown + "...");
        new Thread(() -> {
            ModImporter.Result res = null;
            String err = null;
            try {
                res = ModImporter.importArchive(act, uri, shown, m -> act.runOnUiThread(() -> modsStatus.setText(m)));
            } catch (Throwable e) {
                err = "Import failed: " + e.getMessage();
            }
            final ModImporter.Result r = res;
            final String error = err;
            act.runOnUiThread(() -> {
                importing = false;
                refreshMods();
                if (error != null) {
                    Toast.makeText(act, error, Toast.LENGTH_LONG).show();
                    return;
                }
                Toast.makeText(act, r.message, Toast.LENGTH_LONG).show();
                if (r.anyEnabled) {
                    modsNeedRestart = true;
                    updateModsStatus();
                    confirmRestartForMods(r.message + " Mods are loaded when the game starts. Restart now? "
                            + "Unsaved progress is lost.");
                }
            });
        }, "mod-import").start();
    }

    /** Runs after, but first asks what to do with unapplied checkbox changes. */
    private void leaveMods(Runnable after) {
        if (pendingModChanges() == 0) { after.run(); return; }
        new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Apply mod changes?")
                .setMessage("You changed which mods are on. The game must restart for mods to take effect.")
                .setPositiveButton("Apply", (d, w) -> applyMods(after))
                .setNeutralButton("Discard", (d, w) -> { refreshMods(); after.run(); })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void showMods(boolean show) {
        if (show) refreshMods();
        modsShown = show;
        driversShown = false;
        touchShown = false;
        mainPage.setVisibility(show ? View.GONE : View.VISIBLE);
        modsPage.setVisibility(show ? View.VISIBLE : View.GONE);
        driversPage.setVisibility(View.GONE);
        touchPage.setVisibility(View.GONE);
        graphicsPage.setVisibility(View.GONE);
        graphicsShown = false;
        (show ? modsFocus : firstFocus).requestFocus();
    }

    // ---- drivers page: one driver at a time, through gpu_driver.c's file interface ----
    // Installed drivers are the folders in <internal files>/gpu_drivers/ (meta.json + .so). The choice is the folder
    // name in <external files>/gpu_driver ("system" = the system driver). The driver is opened once at process start,
    // so a new choice takes effect after a restart; activeDriver is what this process started with.

    private String activeDriver = "system";

    private java.io.File selectionFile() {
        java.io.File ext = act.getExternalFilesDir(null);
        return ext == null ? null : new java.io.File(ext, "gpu_driver");
    }

    private String readSelectedDriver() {
        java.io.File f = selectionFile();
        if (f == null || !f.isFile()) return "system";
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f))) {
            String s = r.readLine();
            s = s == null ? "" : s.trim();
            return s.isEmpty() ? "system" : s;
        } catch (java.io.IOException e) {
            return "system";
        }
    }

    private boolean writeSelectedDriver(String name) {
        java.io.File f = selectionFile();
        if (f == null) return false;
        try (java.io.FileWriter w = new java.io.FileWriter(f, false)) {
            w.write(name + "\n");
            return true;
        } catch (java.io.IOException e) {
            return false;
        }
    }

    /** Installed driver folder names: the ones that hold a .so, sorted. */
    private java.util.List<String> installedDrivers() {
        java.util.List<String> out = new java.util.ArrayList<>();
        java.io.File[] dirs = new java.io.File(act.getFilesDir(), "gpu_drivers").listFiles();
        if (dirs != null) for (java.io.File d : dirs) {
            String[] files = d.isDirectory() && !d.getName().startsWith(".") ? d.list() : null;
            if (files == null) continue;
            for (String n : files) if (n.endsWith(".so")) { out.add(d.getName()); break; }
        }
        java.util.Collections.sort(out);
        return out;
    }

    private void buildDrivers() {
        activeDriver = readSelectedDriver(); // what this process starts with (gpu_driver.c read the same file)
        Button back = row("<  Drivers", v -> showDrivers(false));
        back.setTypeface(Typeface.DEFAULT_BOLD);
        driversFocus = back;
        driversPage.addView(back);

        TextView note = text("Pick one GPU driver to run the game with. The system driver is the default. "
                + "A driver that fails to load falls back to the system driver.", 14, false);
        note.setTextColor(0xFFB0B8CC);
        note.setPadding(px(20), px(14), px(20), px(6));
        driversPage.addView(note);

        driverStatus = text("", 13, false);
        driverStatus.setTextColor(0xFFD2D8E8);
        driverStatus.setPadding(px(20), 0, px(20), px(8));
        driversPage.addView(driverStatus);

        driverGroup = new android.widget.RadioGroup(act);
        driverGroup.setOrientation(LinearLayout.VERTICAL);
        driversPage.addView(driverGroup);

        driversPage.addView(row("Import...", v -> act.pickDriverZip()));
        refreshDrivers();
    }

    /**
     * Installs a driver package chosen in the file picker: a .zip holding meta.json and the driver .so (the usual
     * adrenotools layout, at the top or inside one folder). Only those files are kept and every entry is flattened to its
     * file name, so a crafted zip cannot write elsewhere. The folder is named from meta.json's "name" (or the zip's name).
     */
    void importDriver(final android.net.Uri uri) {
        final String shown = displayName(uri);
        Toast.makeText(act, "Importing driver...", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            String msg;
            try {
                msg = installDriverZip(uri, shown);
            } catch (Exception e) {
                msg = "Driver import failed: " + e.getMessage();
            }
            final String m = msg;
            act.runOnUiThread(() -> {
                Toast.makeText(act, m, Toast.LENGTH_LONG).show();
                refreshDrivers();
            });
        }, "driver-import").start();
    }

    private String displayName(android.net.Uri uri) {
        try (android.database.Cursor c = act.getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                if (i >= 0 && c.getString(i) != null) return c.getString(i);
            }
        } catch (Exception ignored) {
        }
        return "driver.zip";
    }

    private static String safeName(String s) {
        s = s.replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9_.-]+", "_").replaceAll("^[._]+", "");
        if (s.length() > 100) s = s.substring(0, 100);
        return s.isEmpty() ? "driver" : s;
    }

    private String installDriverZip(android.net.Uri uri, String shown) throws java.io.IOException {
        java.io.File root = new java.io.File(act.getFilesDir(), "gpu_drivers");
        java.io.File tmp = new java.io.File(root, ".import_tmp");
        deleteTree(tmp);
        if (!tmp.mkdirs()) throw new java.io.IOException("cannot create " + tmp);
        boolean meta = false, so = false;
        try (java.io.InputStream in = act.getContentResolver().openInputStream(uri);
             java.util.zip.ZipInputStream zin = new java.util.zip.ZipInputStream(in)) {
            java.util.zip.ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = zin.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String base = e.getName().substring(e.getName().lastIndexOf('/') + 1);
                boolean isMeta = base.equals("meta.json"), isSo = base.endsWith(".so");
                if (!isMeta && !isSo) continue;
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(new java.io.File(tmp, base))) {
                    int n;
                    while ((n = zin.read(buf)) > 0) out.write(buf, 0, n);
                }
                meta |= isMeta;
                so |= isSo;
            }
        }
        if (!so) {
            deleteTree(tmp);
            return "Not a driver package: no .so inside " + shown + ".";
        }
        String name = safeName(shown);
        java.io.File mj = new java.io.File(tmp, "meta.json");
        if (meta) { // "name" in meta.json, if it has one, names the driver
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(mj))) {
                StringBuilder sb = new StringBuilder();
                String line;
                while ((line = r.readLine()) != null && sb.length() < 8192) sb.append(line);
                java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"name\"\\s*:\\s*\"([^\"]+)\"").matcher(sb);
                if (m.find()) name = safeName(m.group(1));
            }
        }
        java.io.File dest = new java.io.File(root, name);
        deleteTree(dest);
        if (!tmp.renameTo(dest)) throw new java.io.IOException("cannot install into " + dest);
        return "Installed driver " + name + (meta ? "" : " (no meta.json: the first .so is used)") + ".";
    }

    private static void deleteTree(java.io.File f) {
        java.io.File[] kids = f.listFiles();
        if (kids != null) for (java.io.File k : kids) deleteTree(k);
        f.delete();
    }

    /** Rebuilds the list from the folders on disk, with the saved choice checked. */
    private void refreshDrivers() {
        String selected = readSelectedDriver();
        java.util.List<String> names = installedDrivers();
        if (!selected.equals("system") && !names.contains(selected)) selected = "system"; // gone: system is used
        driverGroup.setOnCheckedChangeListener(null);
        driverGroup.removeAllViews();
        addDriverRow("System driver", "system", selected);
        for (String n : names) addDriverRow(n, n, selected);
        driverGroup.setOnCheckedChangeListener((g, id) -> {
            View rb = g.findViewById(id);
            if (rb == null || rb.getTag() == null) return;
            String pick = (String) rb.getTag();
            if (!writeSelectedDriver(pick)) {
                Toast.makeText(act, "Could not save the driver choice.", Toast.LENGTH_SHORT).show();
                return;
            }
            updateDriverStatus(pick);
            if (!pick.equals(activeDriver)) confirm("Restart to apply?",
                    "The new driver is used after the game restarts. Unsaved progress is lost.", "Restart",
                    act::restartGame);
        });
        updateDriverStatus(selected);
    }

    private void updateDriverStatus(String selected) {
        String active = activeDriver.equals("system") ? "System driver" : activeDriver;
        driverStatus.setText(selected.equals(activeDriver) ? "Running: " + active
                : "Running: " + active + "\nTakes effect after a restart.");
    }

    private void addDriverRow(String label, String name, String selected) {
        android.widget.RadioButton rb = new android.widget.RadioButton(act);
        rb.setId(View.generateViewId());
        rb.setTag(name);
        rb.setText(label);
        rb.setTextSize(17);
        rb.setTextColor(Color.WHITE);
        rb.setPadding(px(12), 0, px(20), 0);
        rb.setBackground(focusBg());
        rb.setLayoutParams(new LinearLayout.LayoutParams(-1, px(46)));
        driverGroup.addView(rb);
        if (name.equals(selected)) rb.setChecked(true);
    }

    private void showDrivers(boolean show) {
        if (show) refreshDrivers();
        driversShown = show;
        modsShown = false;
        touchShown = false;
        mainPage.setVisibility(show ? View.GONE : View.VISIBLE);
        modsPage.setVisibility(View.GONE);
        touchPage.setVisibility(View.GONE);
        driversPage.setVisibility(show ? View.VISIBLE : View.GONE);
        graphicsPage.setVisibility(View.GONE);
        graphicsShown = false;
        (show ? driversFocus : firstFocus).requestFocus();
    }

    // ---- on-screen controls page ----

    private void showTouch(boolean show) {
        touchShown = show;
        modsShown = false;
        driversShown = false;
        mainPage.setVisibility(show ? View.GONE : View.VISIBLE);
        modsPage.setVisibility(View.GONE);
        driversPage.setVisibility(View.GONE);
        touchPage.setVisibility(show ? View.VISIBLE : View.GONE);
        graphicsPage.setVisibility(View.GONE);
        graphicsShown = false;
        (show ? touchFocus : firstFocus).requestFocus();
    }

    // ---- graphics page: the GFX drop-downs ----

    private void buildGraphics() {
        Button back = row("<  Graphics", v -> showGraphics(false));
        back.setTypeface(Typeface.DEFAULT_BOLD);
        graphicsFocus = back;
        graphicsPage.addView(back);
        TextView note = text("Lower settings trade picture quality for speed. Options marked \"after restart\" "
                + "take effect the next time the game starts. Console mode Auto: handheld on Mali GPUs, docked elsewhere. "
                + "Anti-aliasing Off is faster on Mali GPUs.", 14, false);
        note.setTextColor(0xFFB0B8CC);
        note.setPadding(px(20), px(14), px(20), px(6));
        graphicsPage.addView(note);
        // drop-downs like the frame rate, applied at once unless marked "after restart"
        for (int gi = 0; gi < GFX.length; gi++) {
            final int i = gi;
            final String title = (String) GFX[i][2];
            final String[] names = (String[]) GFX[i][3];
            Button b = row(title + ": " + names[graphics(i)] + "  ▾", null);
            b.setOnClickListener(v -> {
                android.widget.PopupMenu m = new android.widget.PopupMenu(act, b);
                for (int k = 0; k < names.length; k++) m.getMenu().add(0, k, k, names[k]);
                m.setOnMenuItemClickListener(it -> {
                    int val = it.getItemId();
                    prefs.edit().putInt((String) GFX[i][1], val).apply();
                    SomnielActivity.nativeSetGraphics((Integer) GFX[i][0], val);
                    b.setText(title + ": " + names[val] + "  ▾");
                    return true;
                });
                m.show();
            });
            graphicsPage.addView(b);
        }
    }

    private void showGraphics(boolean show) {
        graphicsShown = show;
        modsShown = false;
        driversShown = false;
        touchShown = false;
        mainPage.setVisibility(show ? View.GONE : View.VISIBLE);
        modsPage.setVisibility(View.GONE);
        driversPage.setVisibility(View.GONE);
        touchPage.setVisibility(View.GONE);
        graphicsPage.setVisibility(show ? View.VISIBLE : View.GONE);
        sv.scrollTo(0, 0); // a page starts at its top (touch mode moves no focus into view)
        (show ? graphicsFocus : firstFocus).requestFocus();
    }

    private android.widget.RadioButton radio(String label) {
        android.widget.RadioButton rb = new android.widget.RadioButton(act);
        rb.setId(View.generateViewId());
        rb.setText(label);
        rb.setTextSize(17);
        rb.setTextColor(Color.WHITE);
        rb.setPadding(px(12), 0, px(20), 0);
        rb.setBackground(focusBg());
        rb.setLayoutParams(new LinearLayout.LayoutParams(-1, px(46)));
        return rb;
    }

    private void buildTouch() {
        Button back = row("<  On-screen controls", v -> showTouch(false));
        back.setTypeface(Typeface.DEFAULT_BOLD);
        touchFocus = back;
        touchPage.addView(back);

        // visibility: always on / always off, plus the automatic switch-off while a controller is connected
        android.widget.RadioGroup power = new android.widget.RadioGroup(act);
        power.setOrientation(LinearLayout.VERTICAL);
        final android.widget.RadioButton on = radio("Always on"), off = radio("Always off");
        power.addView(on);
        power.addView(off);
        (touchEnabled() ? on : off).setChecked(true);
        touchPage.addView(power);
        final CheckBox auto = check("Turn off when a controller is connected", touchHideWithPad(), v -> {
            prefs.edit().putBoolean("touch_hide_with_pad", v).apply();
            act.updateTouchControls();
        });
        touchPage.addView(auto);
        TextView note = text("Built-in controllers on handhelds count as a controller.", 12, false);
        note.setTextColor(0xFFD2D8E8);
        note.setPadding(px(20), 0, px(20), px(10));
        touchPage.addView(note);
        auto.setEnabled(touchEnabled());
        auto.setAlpha(touchEnabled() ? 1f : 0.45f);
        power.setOnCheckedChangeListener((g, id) -> {
            boolean e = id == on.getId();
            prefs.edit().putBoolean("touch_enabled", e).apply();
            auto.setEnabled(e);
            auto.setAlpha(e ? 1f : 0.45f);
            act.updateTouchControls();
        });

        // which left-hand controls are drawn
        touchPage.addView(label("Movement controls"));
        android.widget.RadioGroup move = new android.widget.RadioGroup(act);
        move.setOrientation(LinearLayout.VERTICAL);
        String[] names = {"D-pad and left stick", "D-pad only", "Left stick only"};
        final android.widget.RadioButton[] mb = new android.widget.RadioButton[3];
        for (int i = 0; i < 3; i++) { mb[i] = radio(names[i]); move.addView(mb[i]); }
        mb[touchMoveMode()].setChecked(true);
        move.setOnCheckedChangeListener((g, id) -> {
            for (int i = 0; i < 3; i++) if (mb[i].getId() == id) prefs.edit().putInt("touch_move", i).apply();
            act.updateTouchControls();
        });
        touchPage.addView(move);

        touchPage.addView(check("Fade when idle", touchFade(), v -> {
            prefs.edit().putBoolean("touch_fade", v).apply();
            act.updateTouchControls();
        }));
        TextView fnote = text("Controls fade to almost invisible after a few seconds without a touch and return at the next one.", 12, false);
        fnote.setTextColor(0xFFD2D8E8);
        fnote.setPadding(px(20), 0, px(20), px(10));
        touchPage.addView(fnote);
    }

    // ---- small view helpers ----

    private interface Toggle { void on(boolean checked); }

    private void confirm(String title, String msg, String yes, Runnable action) {
        new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title).setMessage(msg)
                .setPositiveButton(yes, (d, w) -> action.run())
                .setNegativeButton("Cancel", null)
                .show();
    }

    private int px(int dips) { return Math.round(dips * dp); }

    private TextView text(String s, int sp, boolean bold) {
        TextView t = new TextView(act);
        t.setText(s);
        t.setTextColor(Color.WHITE);
        t.setTextSize(sp);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private TextView label(String s) {
        TextView t = text(s, 13, false);
        t.setTextColor(0xFFB0B8CC);
        t.setPadding(px(20), px(8), px(20), px(0));
        return t;
    }

    private StateListDrawable focusBg() {
        StateListDrawable d = new StateListDrawable();
        GradientDrawable hi = new GradientDrawable();
        hi.setColor(0xFF2F3B63);
        GradientDrawable press = new GradientDrawable();
        press.setColor(0xFF3C4B7D);
        d.addState(new int[] {android.R.attr.state_pressed}, press);
        d.addState(new int[] {android.R.attr.state_focused}, hi);
        d.addState(new int[] {}, new ColorDrawable(Color.TRANSPARENT));
        return d;
    }

    private Button row(String s, View.OnClickListener l) {
        Button b = new Button(act);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(17);
        b.setTextColor(Color.WHITE);
        b.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
        b.setPadding(px(20), 0, px(20), 0);
        b.setBackground(focusBg());
        b.setOnClickListener(l);
        b.setLayoutParams(new LinearLayout.LayoutParams(-1, px(46)));
        return b;
    }

    private CheckBox check(String s, boolean on, Toggle t) {
        CheckBox c = new CheckBox(act);
        c.setText(s);
        c.setTextSize(17);
        c.setTextColor(Color.WHITE);
        c.setChecked(on);
        c.setPadding(px(12), 0, px(20), 0);
        c.setBackground(focusBg());
        c.setOnCheckedChangeListener((b, v) -> t.on(v));
        c.setLayoutParams(new LinearLayout.LayoutParams(-1, px(46)));
        return c;
    }
}
