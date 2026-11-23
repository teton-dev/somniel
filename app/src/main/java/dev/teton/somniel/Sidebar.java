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
    private static final int[] FPS_CHOICES = {20, 30, 40, 45, 60};

    /**
     * Graphics options: native key, preference name, row title, choice names, kind. Kind 0: the saved index is the native
     * value (0 = as the game renders). Kind 1: Console mode and Hidden object culling, which have no "Auto": the saved
     * value is the native one (1 = first choice, 2 = second, 0 = never chosen) and the GPU's default is marked "(default)".
     * Kind 2: like 0, but a change needs a restart.
     */
    private static final Object[][] GFX = {
        {4, "gfx_profile", "Console mode", new String[] {"Handheld", "Docked"}, 1},
        {7, "gfx_aspect", "Aspect ratio", new String[] {"Stretch to fill", "16:9"}, 0},
        {0, "gfx_shadows", "Shadows", new String[] {"High", "Medium", "Low", "Off"}, 0},
        {6, "gfx_fxaa", "Anti-aliasing", new String[] {"On", "Off"}, 0},
        {8, "gfx_cull", "Hidden object culling", new String[] {"On", "Off"}, 1},
        {2, "gfx_texbias", "Texture sharpness", new String[] {"Normal", "Softer", "Soft"}, 2},
        {5, "gfx_texsize", "Texture size", new String[] {"Full", "Half", "Quarter"}, 2},
        {3, "gfx_shaders", "Shader precision", new String[] {"Accurate", "Fast", "Fastest, experimental: some text breaks"}, 2},
    };

    private static boolean isMali() { return SomnielActivity.nativeGpuIsMali(); }

    /** The GPU's default choice (index) for the kind 1 options: handheld and culling on for Mali, docked and culling off elsewhere. */
    private static int defaultIndex(int i) { return isMali() ? 0 : 1; }

    /** The saved choice as an index into the names. */
    int graphicsIndex(int i) {
        String[] names = (String[]) GFX[i][3];
        int v = prefs.getInt((String) GFX[i][1], 0);
        if ((Integer) GFX[i][4] == 1) return v == 1 || v == 2 ? v - 1 : defaultIndex(i);
        return v >= 0 && v < names.length ? v : 0;
    }

    /** What the native side gets for an index. */
    private static int nativeValue(int i, int idx) { return (Integer) GFX[i][4] == 1 ? idx + 1 : idx; }

    private static String graphicsName(int i, int idx) {
        String n = ((String[]) GFX[i][3])[idx];
        return (Integer) GFX[i][4] == 1 && idx == defaultIndex(i) ? n + " (default)" : n;
    }

    int graphics(int i) { return nativeValue(i, graphicsIndex(i)); }

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
    private LinearLayout touchPage, graphicsPage;
    private View firstFocus, modsFocus, touchFocus, graphicsFocus;
    private boolean open, modsShown, touchShown, graphicsShown;
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
        {
            GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                    new int[] {0xF71A2135, 0xF70D111C});
            float r = px(18);
            bg.setCornerRadii(new float[] {0, 0, r, r, r, r, 0, 0});
            panel.setBackground(bg);
            panel.setClipToOutline(true);
        }
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
        title.setPadding(px(20), px(12), px(20), px(4));
        panel.addView(title);
        View accent = new View(act);
        GradientDrawable ab = new GradientDrawable();
        ab.setColor(ACCENT);
        ab.setCornerRadius(px(2));
        accent.setBackground(ab);
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(px(32), px(3));
        alp.setMargins(px(20), px(2), 0, px(8));
        panel.addView(accent, alp);

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
        for (int c : FPS_CHOICES) if (f == c) return f;
        return def;
    }
    boolean skipIntro() { return prefs.getBoolean("skip_intro", false); }
    boolean dynamicResolution() { return prefs.getBoolean("dynamic_resolution", true); }
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
        showTouch(false);
        showGraphics(false);
        refreshDual();
        refreshShare();
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
        if (touchShown) { showTouch(false); return true; }
        if (graphicsShown) { showGraphics(false); return true; }
        close();
        return true;
    }

    // ---- main page ----

    private void buildMain() {
        // performance monitor: one slider, Off / 1 / 2 / 3 (like the Steam Deck's), applied at once
        TextView monLabel = label("");
        mainPage.addView(monLabel);
        SeekBar mon = seek();
        mon.setMax(4);
        mon.setKeyProgressIncrement(1);
        mon.setProgress(monitorLevel());
        monLabel.setText(monitorText(monitorLevel()));
        mon.setPadding(px(28), px(8), px(28), px(8));
        mon.setBackground(focusBg());
        mon.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                monLabel.setText(monitorText(p));
                prefs.edit().putInt("monitor_level", p).commit();
                pushMonitors();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        firstFocus = mon;
        mainPage.addView(mon, new LinearLayout.LayoutParams(-1, px(40)));

        // render scale: 50% to 125% in 5% steps; the line under it names the console mode this run uses
        final int steps = (SCALE_MAX - SCALE_MIN) / SCALE_STEP;
        TextView scaleLabel = label("");
        mainPage.addView(scaleLabel);
        SeekBar scale = seek();
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
                prefs.edit().putInt("render_scale", v).commit();
                SomnielActivity.nativeSetRenderScale(v); // re-applied by the game within a frame
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        mainPage.addView(scale, new LinearLayout.LayoutParams(-1, px(40)));
        mainPage.addView(check("Dynamic resolution", dynamicResolution(), on -> {
            prefs.edit().putBoolean("dynamic_resolution", on).commit();
            SomnielActivity.nativeSetDynamicResolution(on);
        }));

        mainPage.addView(divider());

        // frame-rate drop-down: a row that opens a PopupMenu (unlike a Spinner it cannot clip its own text)
        final int[] fpsSel = {frameRate()};
        Button fps = row("", null);
        fps.setText("Frame rate: " + fpsSel[0] + " FPS  ▾");
        fps.setOnClickListener(v -> {
            android.widget.PopupMenu m = new android.widget.PopupMenu(act, fps);
            for (int i = 0; i < FPS_CHOICES.length; i++) m.getMenu().add(0, FPS_CHOICES[i], i, FPS_CHOICES[i] + " FPS");
            m.setOnMenuItemClickListener(it -> {
                fpsSel[0] = it.getItemId();
                prefs.edit().putInt("fps_target", fpsSel[0]).commit();
                SomnielActivity.nativeSetFrameRate(fpsSel[0]); // takes effect at once
                fps.setText("Frame rate: " + fpsSel[0] + " FPS  ▾");
                warnPacing(fpsSel[0]);
                return true;
            });
            m.show();
        });
        mainPage.addView(fps);

        mainPage.addView(row("On-screen controls...", v -> showTouch(true)));

        // dual-screen companion: only listed when the device has a second display (refreshed each time the sidebar opens)
        dualRow = check("Dual-screen companion", true, on -> {
            if (!dualBusy && act.dualScreen() != null) act.dualScreen().setEnabled(on);
        });
        dualNote = text("", 12, false);
        dualNote.setTextColor(0xFFD2D8E8);
        dualNote.setPadding(px(20), 0, px(20), px(10));
        mainPage.addView(dualRow);
        mainPage.addView(dualNote);
        refreshDual();

        mainPage.addView(divider());
        mainPage.addView(row("Graphics...", v -> showGraphics(true)));
        mainPage.addView(row("Mods...", v -> showMods(true)));
        mainPage.addView(row("Import / Export Saves...", v -> new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Import / Export Saves")
                .setMessage("Export: saves all your current saves to a .zip file wherever you choose (the game keeps "
                        + "running).\n\n"
                        + "Import: your current saves are backed up to " + SaveImporter.BACKUP_DIR + " in this device's "
                        + "storage and removed from Somniel, then replaced by the saves you choose: a .zip (such as an "
                        + "export), or the save files themselves (Global, Auto, Manual0, Manual1...; select several at "
                        + "once). The game then restarts and unsaved progress is lost.")
                .setPositiveButton("Export...", (d, w) -> {
                    if (SaveImporter.hasSaves(act)) act.pickSaveExport(SaveImporter.exportName());
                    else Toast.makeText(act, "There are no saves to export yet.", Toast.LENGTH_LONG).show();
                })
                .setNeutralButton("Import...", (d, w) -> act.pickSaves())
                .setNegativeButton("Cancel", null)
                .show()));

        mainPage.addView(divider());
        mainPage.addView(check("Skip intro", skipIntro(), on -> {
            prefs.edit().putBoolean("skip_intro", on).commit();
            SomnielActivity.nativeSetSkipIntro(on);
        }));
        if (act.telemetry() != null && act.telemetry().available()) {
            Telemetry tel = act.telemetry();
            shareRow = check("Share anonymous performance & crash data", tel.enabled(), on -> { if (!shareBusy) tel.setConsent(on); });
            mainPage.addView(shareRow);
        }
        mainPage.addView(row("Save diagnostic log", v -> new Thread(() ->
                LogExporter.exportLog(act, LogExporter.logPath(act), "somniel_manual", "Saved by the user from the sidebar")).start()));
        mainPage.addView(check("Check for updates at startup", act.updateCheckEnabled(), on -> act.setUpdateCheckEnabled(on)));
        mainPage.addView(row("Check for updates", v -> act.checkForUpdates(true)));
        mainPage.addView(divider());
        mainPage.addView(row("Restart Game", v -> confirm("Restart the game?",
                "The game restarts at the title screen. Unsaved progress is lost.", "Restart", act::restartGame)));
        Button quit = row("Quit Game", v -> confirm("Quit the game?",
                "Unsaved progress is lost.", "Quit", act::quitGame));
        quit.setTextColor(0xFFFF8A94);
        mainPage.addView(quit);
    }

    /**
     * A frame rate is only evenly paced when the screen refreshes at a whole multiple of it; on any other rate frames
     * alternate between being shown for different numbers of refreshes, which judders.
     *
     * Screens that switch their refresh rate by themselves (adaptive / variable refresh: the current mode lists
     * seamless alternative rates) are judged by the whole range they can reach. A floor of 40 Hz or less means a
     * variable-refresh panel that can follow nearly any frame rate: no warning. Above that, a warning only when the
     * chosen frame rate divides none of the rates the screen can switch to.
     *
     * Fixed-rate screens keep the original rule: 20 FPS wants 60 or 120 Hz, 45 FPS wants 90 Hz; the warning says
     * whether the screen has such a setting the user can pick in Android's display settings.
     */
    private void warnPacing(int fps) {
        android.view.Display d = act.getWindowManager().getDefaultDisplay();
        java.util.TreeSet<Float> reach = adaptiveRates(d);
        if (reach != null) {
            float floor = reach.first(), ceil = reach.last();
            if (floor <= 40.5f) return; // variable refresh down to 40 Hz or less: paced at whatever rate is chosen
            for (float hz : reach) if (divides(fps, hz)) return;
            StringBuilder list = new StringBuilder();
            for (float hz : reach) list.append(list.length() > 0 ? ", " : "").append(Math.round(hz));
            new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Uneven frame pacing")
                    .setMessage(fps + " FPS will be unevenly paced on this screen: it switches its refresh rate between "
                            + Math.round(floor) + " and " + Math.round(ceil) + " Hz (" + list + " Hz), and " + fps
                            + " does not divide evenly into any of them.")
                    .setPositiveButton("OK", null)
                    .show();
            return;
        }
        int[] fit = fps == 20 ? new int[] {60, 120} : fps == 45 ? new int[] {90} : null;
        if (fit == null) return;
        float now = d.getRefreshRate();
        if (matches(now, fit)) return;
        boolean available = false;
        for (android.view.Display.Mode m : d.getSupportedModes()) if (matches(m.getRefreshRate(), fit)) available = true;
        String rates = fit.length == 1 ? fit[0] + " Hz" : fit[0] + " or " + fit[1] + " Hz";
        String msg = fps + " FPS will be unevenly paced on this screen at its current setting (" + Math.round(now)
                + " Hz): it is only smooth at " + rates + ".";
        msg += available
                ? "\n\nThis screen has a " + rates + " option: turn it on in Android's display settings (screen refresh "
                        + "rate or motion smoothness)."
                : "\n\nThis screen does not offer " + rates + ".";
        new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Uneven frame pacing")
                .setMessage(msg)
                .setPositiveButton("OK", null)
                .show();
    }

    /**
     * The refresh rates the screen can switch to by itself at the current resolution, or null when it has only one (a
     * fixed rate). Seamless switching shows as the current mode's alternative refresh rates (Android 12+); before that,
     * as several modes at the same size.
     */
    private static java.util.TreeSet<Float> adaptiveRates(android.view.Display d) {
        android.view.Display.Mode cur = d.getMode();
        java.util.TreeSet<Float> rates = new java.util.TreeSet<>();
        rates.add(cur.getRefreshRate());
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            for (float hz : cur.getAlternativeRefreshRates()) rates.add(hz);
        } else {
            for (android.view.Display.Mode m : d.getSupportedModes())
                if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight())
                    rates.add(m.getRefreshRate());
        }
        // rates within 1 Hz of each other (59.94 and 60) are one rate
        java.util.TreeSet<Float> out = new java.util.TreeSet<>();
        for (float hz : rates) if (out.isEmpty() || hz - out.last() > 1f) out.add(hz);
        return out.size() > 1 ? out : null;
    }

    /** The refresh rate is a whole multiple of the frame rate (each frame shown for the same number of refreshes). */
    private static boolean divides(int fps, float hz) {
        float k = hz / fps;
        return Math.round(k) >= 1 && Math.abs(k - Math.round(k)) < 0.03f;
    }

    private static boolean matches(float hz, int[] rates) {
        for (int r : rates) if (Math.abs(hz - r) < 1.5f) return true;
        return false;
    }

    /** "Render scale: 100%  (1728×972)" with the console mode this run uses in a smaller, lighter second line. */
    private CharSequence scaleText(int v) {
        // the game's own 0.9 scale of 1280x720 (handheld, 648p) or 1920x1080 (docked) is the 100% size
        boolean hh = SomnielActivity.nativeIsHandheld();
        int bw = hh ? 1152 : 1728, bh = hh ? 648 : 972;
        String l1 = "Render scale: " + v + "%  (" + Math.round(bw * v / 100f) + "×" + Math.round(bh * v / 100f) + ")";
        String l2 = hh ? "Handheld mode" : "Docked mode";
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
    private CheckBox shareRow;
    private boolean shareBusy;

    /** The share-data box follows the saved answer, e.g. the first-launch prompt's Yes (the box is built before it is asked). */
    private void refreshShare() {
        if (shareRow == null || act.telemetry() == null) return;
        shareBusy = true;
        shareRow.setChecked(act.telemetry().enabled());
        shareBusy = false;
    }

    private CheckBox dualRow;
    private TextView dualNote;
    private boolean dualBusy;

    /** Shows the dual-screen option only on a device with a second display, and keeps it in step with the saved choice. */
    private void refreshDual() {
        DualScreen d = act.dualScreen();
        boolean has = d != null && d.hasSecondDisplay();
        int vis = has ? View.VISIBLE : View.GONE;
        dualRow.setVisibility(vis);
        dualNote.setVisibility(vis);
        if (!has) return;
        dualBusy = true;
        dualRow.setChecked(d.enabled());
        dualBusy = false;
        dualNote.setText(d.hasPackage()
                ? "Shows the battle map, unit details and rosters on the second screen."
                : "No companion package is installed for this game.");
    }

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

        // Cobalt (the mod loader) is fetched by the user: download the latest release or import a release.zip
        cobaltStatus = text("", 14, false);
        cobaltStatus.setTextColor(0xFFD2D8E8);
        cobaltStatus.setPadding(px(20), px(12), px(20), px(4));
        modsPage.addView(cobaltStatus);
        cobaltDownloadBtn = row("Download latest Cobalt", v -> cobaltDownload(false));
        cobaltDownloadBtn.setTypeface(Typeface.DEFAULT_BOLD);
        modsPage.addView(cobaltDownloadBtn);
        modsPage.addView(row("Import Cobalt zip...", v -> leaveMods(act::pickCobaltZip)));
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
                prefs.edit().putBoolean("cobalt_declined", false).commit();
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

    /**
     * Checks and stages the saves chosen in the file picker (SaveImporter), shows what was found, and on Restart marks
     * them to be applied (backup, then swap) when the game starts again.
     */
    void importSaves(final java.util.List<android.net.Uri> uris) {
        if (importing) return;
        importing = true;
        final Toast t = Toast.makeText(act, "Reading the saves...", Toast.LENGTH_SHORT);
        t.show();
        new Thread(() -> {
            SaveImporter.Staged s = null;
            String err = null;
            try {
                s = SaveImporter.stage(act, uris);
            } catch (Throwable e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final SaveImporter.Staged got = s;
            final String why = err;
            act.runOnUiThread(() -> {
                importing = false;
                if (got == null) {
                    new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle("Could not import saves").setMessage("Nothing was changed: " + why + ".")
                            .setPositiveButton("OK", null).show();
                    return;
                }
                StringBuilder names = new StringBuilder();
                for (String n : got.names) names.append(names.length() > 0 ? ", " : "").append(n);
                new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle("Import " + got.names.size() + " save file" + (got.names.size() == 1 ? "" : "s") + "?")
                        .setMessage(names + "\n\nThe game restarts, backs up your current saves to "
                                + SaveImporter.BACKUP_DIR + ", removes them and loads these. Unsaved progress is lost.")
                        .setPositiveButton("Restart", (d, w) -> {
                            try {
                                SaveImporter.commit(act);
                                act.restartGame();
                            } catch (java.io.IOException e) {
                                SaveImporter.discard(act);
                                Toast.makeText(act, "Could not import: " + e.getMessage(), Toast.LENGTH_LONG).show();
                            }
                        })
                        .setNegativeButton("Cancel", (d, w) -> SaveImporter.discard(act))
                        .setOnCancelListener(d -> SaveImporter.discard(act))
                        .show();
            });
        }).start();
    }

    /** Writes the saves as a zip to the document the user just created in the file picker (SaveImporter.exportTo). */
    void exportSaves(final android.net.Uri uri) {
        if (importing) return;
        importing = true;
        Toast.makeText(act, "Exporting the saves...", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            int n = 0;
            String err = null;
            try {
                n = SaveImporter.exportTo(act, uri);
            } catch (Throwable e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final int count = n;
            final String why = err;
            act.runOnUiThread(() -> {
                importing = false;
                if (why != null) {
                    new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle("Could not export saves").setMessage("Nothing was exported: " + why + ".")
                            .setPositiveButton("OK", null).show();
                    return;
                }
                Toast.makeText(act, "Exported " + count + " save file" + (count == 1 ? "" : "s") + ".", Toast.LENGTH_LONG).show();
            });
        }, "save-export").start();
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
        touchShown = false;
        mainPage.setVisibility(show ? View.GONE : View.VISIBLE);
        modsPage.setVisibility(show ? View.VISIBLE : View.GONE);
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
                refreshDriverRow();
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

    // ---- on-screen controls page ----

    private void showTouch(boolean show) {
        touchShown = show;
        modsShown = false;
        mainPage.setVisibility(show ? View.GONE : View.VISIBLE);
        modsPage.setVisibility(View.GONE);
        touchPage.setVisibility(show ? View.VISIBLE : View.GONE);
        graphicsPage.setVisibility(View.GONE);
        graphicsShown = false;
        (show ? touchFocus : firstFocus).requestFocus();
    }

    // ---- graphics page: the GFX drop-downs ----

    // staged choices: picked in the drop-downs, saved and sent to the game by "Apply settings"
    private int[] gfxPending;         // index per GFX row
    private int[] gfxStarted;         // native value each row had when this process started (restart options compare to it)
    private String driverPending;     // GPU driver folder name ("system" = the system driver)
    private Button[] gfxRows;
    private Button driverRow;

    private void buildGraphics() {
        gfxPending = new int[GFX.length];
        gfxStarted = new int[GFX.length];
        gfxRows = new Button[GFX.length];
        for (int i = 0; i < GFX.length; i++) gfxStarted[i] = graphics(i);
        activeDriver = readSelectedDriver(); // what this process starts with (gpu_driver.c read the same file)

        Button back = row("<  Graphics", v -> showGraphics(false));
        back.setTypeface(Typeface.DEFAULT_BOLD);
        graphicsFocus = back;
        graphicsPage.addView(back);
        TextView note = text("Pick your settings, then tap Apply settings.", 14, false);
        note.setTextColor(0xFFB0B8CC);
        note.setPadding(px(20), px(14), px(20), px(6));
        graphicsPage.addView(note);
        // drop-downs like the frame rate
        for (int gi = 0; gi < GFX.length; gi++) {
            final int i = gi;
            final String title = (String) GFX[i][2];
            final String[] names = (String[]) GFX[i][3];
            final Button b = row("", null);
            gfxRows[i] = b;
            b.setOnClickListener(v -> {
                android.widget.PopupMenu m = new android.widget.PopupMenu(act, b);
                for (int k = 0; k < names.length; k++) m.getMenu().add(0, k, k, graphicsName(i, k));
                m.setOnMenuItemClickListener(it -> {
                    gfxPending[i] = it.getItemId();
                    b.setText(title + ": " + graphicsName(i, gfxPending[i]) + "  ▾");
                    return true;
                });
                m.show();
            });
            graphicsPage.addView(b);
        }
        // GPU driver: the system one or an installed package
        driverRow = row("", null);
        driverRow.setOnClickListener(v -> {
            android.widget.PopupMenu m = new android.widget.PopupMenu(act, driverRow);
            java.util.List<String> names = installedDrivers();
            m.getMenu().add(0, 0, 0, "System driver");
            for (int k = 0; k < names.size(); k++) m.getMenu().add(0, k + 1, k + 1, names.get(k));
            m.setOnMenuItemClickListener(it -> {
                driverPending = it.getItemId() == 0 ? "system" : names.get(it.getItemId() - 1);
                refreshDriverRow();
                return true;
            });
            m.show();
        });
        graphicsPage.addView(driverRow);
        graphicsPage.addView(row("Import driver...", v -> act.pickDriverZip()));

        Button apply = row("Apply settings", v -> applyGraphics());
        apply.setTypeface(Typeface.DEFAULT_BOLD);
        graphicsPage.addView(apply);
        refreshGraphicsRows();
    }

    /** Shows the saved choices (drops anything staged but not applied). */
    private void refreshGraphicsRows() {
        for (int i = 0; i < GFX.length; i++) {
            gfxPending[i] = graphicsIndex(i);
            gfxRows[i].setText(GFX[i][2] + ": " + graphicsName(i, gfxPending[i]) + "  ▾");
        }
        driverPending = readSelectedDriver();
        if (!driverPending.equals("system") && !installedDrivers().contains(driverPending)) driverPending = "system";
        refreshDriverRow();
    }

    private void refreshDriverRow() {
        if (driverRow == null) return;
        if (driverPending == null) driverPending = readSelectedDriver();
        driverRow.setText("GPU driver: " + (driverPending.equals("system") ? "System driver" : driverPending) + "  ▾");
    }

    /** Saves the staged choices, sends them to the game, and offers a restart when one of them needs it. */
    private void applyGraphics() {
        boolean restart = false;
        SharedPreferences.Editor e = prefs.edit();
        for (int i = 0; i < GFX.length; i++) {
            int nv = nativeValue(i, gfxPending[i]);
            // the saved value: the native one for the options without Auto, else the index
            e.putInt((String) GFX[i][1], (Integer) GFX[i][4] == 1 ? nv : gfxPending[i]);
            SomnielActivity.nativeSetGraphics((Integer) GFX[i][0], nv);
            if ((Integer) GFX[i][4] != 0 && nv != gfxStarted[i]) restart = true;
        }
        e.commit();
        if (!writeSelectedDriver(driverPending)) {
            Toast.makeText(act, "Could not save the driver choice.", Toast.LENGTH_SHORT).show();
            driverPending = readSelectedDriver();
            refreshDriverRow();
        } else if (!driverPending.equals(activeDriver)) {
            restart = true;
        }
        if (!restart) {
            Toast.makeText(act, "Settings applied.", Toast.LENGTH_SHORT).show();
            return;
        }
        new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Restart to apply?")
                .setMessage("Some of these settings take effect when the game restarts. Unsaved progress is lost.")
                .setPositiveButton("Restart", (d, w) -> act.restartGame())
                .setNegativeButton("Not now", null)
                .show();
    }

    private void showGraphics(boolean show) {
        if (show) refreshGraphicsRows();
        graphicsShown = show;
        modsShown = false;
        touchShown = false;
        mainPage.setVisibility(show ? View.GONE : View.VISIBLE);
        modsPage.setVisibility(View.GONE);
        touchPage.setVisibility(View.GONE);
        graphicsPage.setVisibility(show ? View.VISIBLE : View.GONE);
        sv.scrollTo(0, 0); // a page starts at its top (touch mode moves no focus into view)
        (show ? graphicsFocus : firstFocus).requestFocus();
    }

    private android.widget.RadioButton radio(String label) {
        android.widget.RadioButton rb = new android.widget.RadioButton(act);
        rb.setId(View.generateViewId());
        rb.setText(label);
        rb.setTextSize(16);
        rb.setTextColor(Color.WHITE);
        rb.setButtonTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        rb.setPadding(px(8), 0, px(14), 0);
        rb.setBackground(focusBg());
        rb.setLayoutParams(rowParams());
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
            prefs.edit().putBoolean("touch_hide_with_pad", v).commit();
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
            prefs.edit().putBoolean("touch_enabled", e).commit();
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
            for (int i = 0; i < 3; i++) if (mb[i].getId() == id) prefs.edit().putInt("touch_move", i).commit();
            act.updateTouchControls();
        });
        touchPage.addView(move);

        touchPage.addView(check("Fade when idle", touchFade(), v -> {
            prefs.edit().putBoolean("touch_fade", v).commit();
            act.updateTouchControls();
        }));
        TextView fnote = text("Controls fade to almost invisible after a few seconds without a touch and return at the next one.", 12, false);
        fnote.setTextColor(0xFFD2D8E8);
        fnote.setPadding(px(20), 0, px(20), px(10));
        touchPage.addView(fnote);

        // appearance: overall size and opacity, applied at once (the controls themselves sit behind the panel)
        final TouchControls tc = act.touchControls();
        touchPage.addView(label("Appearance"));
        final TextView sizeLabel = label("");
        touchPage.addView(sizeLabel);
        final int sizeSteps = (TouchControls.SIZE_MAX - TouchControls.SIZE_MIN) / 5;
        SeekBar size = seek();
        size.setMax(sizeSteps);
        size.setKeyProgressIncrement(1);
        size.setProgress((tc.sizePct() - TouchControls.SIZE_MIN) / 5);
        sizeLabel.setText("Size: " + tc.sizePct() + "%");
        size.setPadding(px(28), px(8), px(28), px(8));
        size.setBackground(focusBg());
        size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                int v = TouchControls.SIZE_MIN + p * 5;
                sizeLabel.setText("Size: " + v + "%");
                tc.setSizePct(v);
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        touchPage.addView(size, new LinearLayout.LayoutParams(-1, px(40)));

        final TextView opLabel = label("");
        touchPage.addView(opLabel);
        final int opSteps = (100 - TouchControls.OPACITY_MIN) / 5;
        SeekBar op = seek();
        op.setMax(opSteps);
        op.setKeyProgressIncrement(1);
        op.setProgress((tc.opacityPct() - TouchControls.OPACITY_MIN) / 5);
        opLabel.setText("Opacity: " + tc.opacityPct() + "%");
        op.setPadding(px(28), px(8), px(28), px(8));
        op.setBackground(focusBg());
        op.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                int v = TouchControls.OPACITY_MIN + p * 5;
                opLabel.setText("Opacity: " + v + "%");
                tc.setOpacityPct(v);
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        touchPage.addView(op, new LinearLayout.LayoutParams(-1, px(40)));

        // layout: drag every control to where it suits, resize each one on its own
        touchPage.addView(label("Layout"));
        touchPage.addView(row("Edit layout...", v -> {
            close();
            act.startTouchEdit();
        }));
        TextView lnote = text("Drag a control to move it. Tap one, then use Smaller / Bigger to resize just that one. "
                + "Back finishes.", 12, false);
        lnote.setTextColor(0xFFD2D8E8);
        lnote.setPadding(px(20), 0, px(20), px(6));
        touchPage.addView(lnote);
        touchPage.addView(row("Reset layout", v -> confirm("Reset the layout?",
                "Every control goes back to its original place and size. Overall size and opacity are kept.", "Reset",
                tc::resetLayout)));
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

    // ---- skin: the launcher icon's red as the accent, rounded inset highlights, compact rows ----

    private static final int ACCENT = 0xFFC8283C;
    private static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);

    private TextView label(String s) {
        TextView t = text(s, 13, false);
        t.setTextColor(0xFFA9B2C9);
        t.setLetterSpacing(0.02f);
        t.setPadding(px(20), px(8), px(20), px(0));
        return t;
    }

    /** A thin line between groups of rows. */
    private View divider() {
        View v = new View(act);
        v.setBackgroundColor(0x1FFFFFFF);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, Math.max(1, px(1)));
        lp.setMargins(px(20), px(6), px(20), px(6));
        v.setLayoutParams(lp);
        return v;
    }

    /** Rows sit 8 dp in from the panel's edges, so their highlight reads as a rounded pill. */
    private LinearLayout.LayoutParams rowParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, px(42));
        lp.setMargins(px(8), px(1), px(8), px(1));
        return lp;
    }

    /** A slider drawn by us (a thin rounded track, a red fill, a white knob): tints alone are ignored by some vendor
     *  themes (Samsung's One UI keeps its own yellow). */
    private SeekBar seek() {
        SeekBar s = new SeekBar(act);
        float r = px(3);
        GradientDrawable track = new GradientDrawable();
        track.setColor(0x40FFFFFF);
        track.setCornerRadius(r);
        GradientDrawable fill = new GradientDrawable();
        fill.setColor(ACCENT);
        fill.setCornerRadius(r);
        android.graphics.drawable.LayerDrawable bar = new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[] {
                track, new android.graphics.drawable.ClipDrawable(fill, Gravity.START, android.graphics.drawable.ClipDrawable.HORIZONTAL)});
        bar.setId(0, android.R.id.background);
        bar.setId(1, android.R.id.progress);
        // the track's height: the bar is centred in the view, so inset it top and bottom to a 6 dp line
        s.setProgressDrawable(bar);
        s.setMinHeight(px(6));
        s.setMaxHeight(px(6));
        GradientDrawable knob = new GradientDrawable();
        knob.setShape(GradientDrawable.OVAL);
        knob.setColor(0xFFFFFFFF);
        knob.setStroke(px(2), ACCENT);
        knob.setSize(px(18), px(18));
        s.setThumb(knob);
        s.setSplitTrack(false);
        return s;
    }

    private StateListDrawable focusBg() {
        StateListDrawable d = new StateListDrawable();
        GradientDrawable hi = new GradientDrawable();
        hi.setColor(0xFF27324F);
        hi.setCornerRadius(px(10));
        hi.setStroke(Math.max(1, px(1)), 0x66C8283C);
        GradientDrawable press = new GradientDrawable();
        press.setColor(0xFF33416A);
        press.setCornerRadius(px(10));
        d.addState(new int[] {android.R.attr.state_pressed}, press);
        d.addState(new int[] {android.R.attr.state_focused}, hi);
        d.addState(new int[] {}, new ColorDrawable(Color.TRANSPARENT));
        return d;
    }

    /** A small stroked chevron for rows that open a page ("Graphics...") or go back ("<  Mods"). */
    private final class Chevron extends android.graphics.drawable.Drawable {
        private final boolean back;
        private final android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        Chevron(boolean back) {
            this.back = back;
            p.setStyle(android.graphics.Paint.Style.STROKE);
            p.setStrokeWidth(px(2));
            p.setStrokeCap(android.graphics.Paint.Cap.ROUND);
            p.setStrokeJoin(android.graphics.Paint.Join.ROUND);
            p.setColor(back ? 0xFFFFFFFF : 0x99FFFFFF);
            setBounds(0, 0, px(10), px(14));
        }
        @Override public void draw(android.graphics.Canvas c) {
            android.graphics.Rect b = getBounds();
            float x0 = b.left + px(2), x1 = b.right - px(2), y0 = b.top + px(2), y1 = b.bottom - px(2), ym = b.exactCenterY();
            android.graphics.Path path = new android.graphics.Path();
            if (back) { path.moveTo(x1, y0); path.lineTo(x0, ym); path.lineTo(x1, y1); }
            else { path.moveTo(x0, y0); path.lineTo(x1, ym); path.lineTo(x0, y1); }
            c.drawPath(path, p);
        }
        @Override public int getIntrinsicWidth() { return px(10); }
        @Override public int getIntrinsicHeight() { return px(14); }
        @Override public void setAlpha(int a) { p.setAlpha(a); }
        @Override public void setColorFilter(android.graphics.ColorFilter f) { p.setColorFilter(f); }
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    private Button row(String s, View.OnClickListener l) {
        Button b = new Button(act);
        b.setAllCaps(false);
        b.setTextSize(16);
        b.setTypeface(MEDIUM);
        b.setTextColor(Color.WHITE);
        b.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
        b.setPadding(px(14), 0, px(14), 0);
        b.setStateListAnimator(null); // flat: no raised shadow when pressed
        b.setBackground(focusBg());
        b.setOnClickListener(l);
        b.setLayoutParams(rowParams());
        b.setCompoundDrawablePadding(px(10));
        if (s.startsWith("<  ")) { // a page's back row: a chevron in place of the "<"
            s = s.substring(3);
            b.setCompoundDrawablesRelative(new Chevron(true), null, null, null);
        } else if (s.endsWith("...")) { // opens a page: the name, and a chevron at the far end
            s = s.substring(0, s.length() - 3);
            b.setCompoundDrawablesRelative(null, null, new Chevron(false), null);
        }
        b.setText(s);
        return b;
    }

    private CheckBox check(String s, boolean on, Toggle t) {
        CheckBox c = new CheckBox(act);
        c.setText(s);
        c.setTextSize(16);
        c.setTextColor(Color.WHITE);
        c.setButtonTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        c.setChecked(on);
        c.setPadding(px(8), 0, px(14), 0);
        c.setBackground(focusBg());
        c.setOnCheckedChangeListener((b, v) -> t.on(v));
        c.setLayoutParams(rowParams());
        return c;
    }
}
