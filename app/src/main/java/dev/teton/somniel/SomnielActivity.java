package dev.teton.somniel;

import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.widget.ImageView;

import org.libsdl.app.SDLActivity;

public class SomnielActivity extends SDLActivity {
    private Sidebar sidebar;
    private SetupDialog setup;

    /** The sidebar is open: the game's input is held at neutral (sidebar.c). */
    static native void nativeSidebarOpen(boolean open);
    /** Frame rate 30, 40 or 60 and render scale in percent (50 to 125): applied live (game_patches.c). */
    static native void nativeSetFrameRate(int fps);
    static native void nativeSetRenderScale(int pct);
    /** The frame rate used until the user picks one: 30 on the handheld profile (Mali GPUs), else 60 (sidebar.c). */
    static native int nativeDefaultFrameRate();
    /** Graphics options (game_patches.c): key 0 shadows (0 High .. 3 Off), 1 model detail (0 Normal .. 2 Low),
     *  2 texture sharpness (0 Normal .. 2 Soft). 0 everywhere renders as the game does. */
    static native void nativeSetGraphics(int key, int value);
    /** The performance monitor level the overlay draws: 0 off, 1 FPS, 2 + frame times and graph, 3 + temperatures (overlay.c). */
    static native void nativeSetMonitorLevel(int level);

    /** The touch controls' state: button bits (main.c NP_*) and both sticks, -32767..32767 with up positive. */
    static native void nativeTouchPad(long buttons, int lx, int ly, int rx, int ry);

    /** The text dialog's answer: the entered text, or null when cancelled (hooks_swkbd.c). */
    static native void nativeTextResult(String text);

    /**
     * nn::swkbd::ShowKeyboard (the player's name in New Game): an OK / Cancel dialog with a text field. Called from the
     * native side on a host thread; answers through nativeTextResult exactly once.
     */
    public void requestText(final String initial, final int min, final int max, final boolean numeric) {
        runOnUiThread(() -> {
            final android.widget.EditText edit = new android.widget.EditText(this);
            edit.setSingleLine(true);
            edit.setText(initial);
            edit.setSelection(edit.getText().length());
            edit.setInputType(numeric ? android.text.InputType.TYPE_CLASS_NUMBER
                    : android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            if (max > 0) edit.setFilters(new android.text.InputFilter[] { new android.text.InputFilter.LengthFilter(max) });
            edit.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE | android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            final boolean[] done = { false };
            final android.app.AlertDialog d = new android.app.AlertDialog.Builder(this,
                    android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(numeric ? "Enter a number" : "Enter a name")
                    .setView(edit)
                    .setCancelable(true)
                    .setPositiveButton(android.R.string.ok, null) // set below so an invalid entry keeps it open
                    .setNegativeButton(android.R.string.cancel, null)
                    .create();
            final Runnable accept = () -> {
                if (done[0]) return;
                String t = edit.getText().toString();
                if (t.length() < min) return;
                done[0] = true;
                d.dismiss();
                nativeTextResult(t);
            };
            d.setOnCancelListener(x -> { if (!done[0]) { done[0] = true; nativeTextResult(null); } });
            d.setOnDismissListener(x -> { if (!done[0]) { done[0] = true; nativeTextResult(null); } });
            d.setOnShowListener(x -> {
                d.getButton(android.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> accept.run());
                d.getButton(android.app.AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> {
                    if (done[0]) return;
                    done[0] = true;
                    d.dismiss();
                    nativeTextResult(null);
                });
                edit.requestFocus();
            });
            edit.setOnEditorActionListener((v, actionId, ev) -> { accept.run(); return true; });
            if (d.getWindow() != null) {
                d.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
            }
            d.show();
        });
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "somniel" };
    }

    /**
     * Edge-to-edge, system bars hidden. SDL only does this from a message posted after its native window exists, which
     * races the first layout: when it loses, the surface is laid out in the area minus the 55 px system bar and the
     * game shows black bars top and bottom (intermittent). Applied on create, resume and every focus gain, before
     * the window is first measured.
     */
    private void goImmersive() {
        android.view.Window w = getWindow();
        if (w == null) return;
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            w.getAttributes().layoutInDisplayCutoutMode =
                    android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.hide(android.view.WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                    | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) goImmersive();
    }

    @Override
    protected void onResume() {
        super.onResume();
        goImmersive();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        goImmersive();
        showSplash(); // first, so the setup dialog, sidebar and notices all draw above it
        telemetry = new Telemetry(this); // before the sidebar: it shows the opt-in toggle
        touch = new TouchControls(this); // below the sidebar, above the game
        touch.setListener(new TouchControls.Listener() {
            @Override public void onPad(long b, int lx, int ly, int rx, int ry) { nativeTouchPad(b, lx, ly, rx, ry); }
        });
        addContentView(touch, new ViewGroup.LayoutParams(-1, -1));
        sidebar = new Sidebar(this);
        addContentView(sidebar.view(), new ViewGroup.LayoutParams(-1, -1));
        setup = new SetupDialog(this);
        addContentView(setup.view(), new ViewGroup.LayoutParams(-1, -1));
        if (!SetupDialog.gameFilesPresent(this)) setup.show(); // first boot: ask for the user's game files
        updateTouchControls();
        android.hardware.input.InputManager im = (android.hardware.input.InputManager) getSystemService(INPUT_SERVICE);
        im.registerInputDeviceListener(new android.hardware.input.InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) { updateTouchControls(); }
            @Override public void onInputDeviceRemoved(int id) { updateTouchControls(); }
            @Override public void onInputDeviceChanged(int id) { updateTouchControls(); }
        }, new Handler(Looper.getMainLooper()));
        nativeSetMonitorLevel(sidebar.monitorLevel());
        nativeSetFrameRate(sidebar.frameRate());
        nativeSetRenderScale(sidebar.renderScale());
        sidebar.pushGraphics();

        getWindow().getDecorView().post(() -> {
            // boot notices: the untested-GPU warning first, then the one-time telemetry question
            Runnable ask = () -> {
                if (telemetry.available() && telemetry.consent() == null) telemetry.showConsent(this::offerCobalt);
                else { telemetry.start(); offerCobalt(); }
            };
            if (telemetry.untestedGpu()) telemetry.showGpuWarning(ask); else ask.run();
        });
    }

    /**
     * Cobalt (the mod loader) is not bundled: when it is missing, ask once whether to fetch it. "Not now" is remembered;
     * the Mods page always has the buttons.
     */
    private void offerCobalt() {
        if (CobaltInstaller.installed(this) || sidebar == null || (setup != null && setup.isShowing())) return;
        final android.content.SharedPreferences p = getSharedPreferences("sidebar", MODE_PRIVATE);
        if (p.getBoolean("cobalt_declined", false)) return;
        new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Get Cobalt for mods?")
                .setMessage("Mods need Cobalt, a mod loader by Raytwo (github.com/Raytwo/Cobalt). It is not included "
                        + "with Somniel, but it can be downloaded from GitHub now, or imported from a release.zip you "
                        + "already have. You can do this later from the sidebar: Mods.")
                .setPositiveButton("Download", (d, w) -> sidebar.cobaltDownload(true))
                .setNeutralButton("Import file...", (d, w) -> pickCobaltZip())
                .setNegativeButton("Not now", (d, w) -> p.edit().putBoolean("cobalt_declined", true).apply())
                .show();
    }

    private Telemetry telemetry;
    private TouchControls touch;
    private ImageView splash;

    /** A real controller is attached (a built-in one on a handheld counts): not a virtual or on-screen device. */
    private static boolean gamepadConnected() {
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int src = d.getSources();
            if ((src & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
                    || (src & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK)
                return true;
        }
        return false;
    }

    /** Shows or hides the on-screen controls: Auto = while no controller is connected; or always on / off (sidebar). */
    void updateTouchControls() {
        if (touch == null || sidebar == null) return;
        boolean show = sidebar.touchEnabled() && !(sidebar.touchHideWithPad() && gamepadConnected());
        int mv = sidebar.touchMoveMode();
        touch.configure(mv != 2, mv != 1, sidebar.touchFade());
        touch.setVisibility(show ? android.view.View.VISIBLE : android.view.View.GONE);
    }

    static native boolean nativeGameHasFrames();

    /** Sommie on navy covers the surface until the game presents its first frame (the native side only has a dark clear then). */
    private void showSplash() {
        splash = new ImageView(this);
        splash.setBackgroundColor(0xFF1B2545);
        splash.setImageResource(R.drawable.splash_icon);
        splash.setScaleType(ImageView.ScaleType.FIT_CENTER);
        splash.setPadding(0, 0, 0, 0);
        addContentView(splash, new ViewGroup.LayoutParams(-1, -1));
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() {
            @Override public void run() {
                if (splash == null) return;
                if (nativeGameHasFrames()) {
                    splash.animate().alpha(0f).setDuration(250).withEndAction(() -> {
                        ((ViewGroup) splash.getParent()).removeView(splash);
                        splash = null;
                    }).start();
                } else {
                    h.postDelayed(this, 100);
                }
            }
        });
    }

    Telemetry telemetry() { return telemetry; }

    @Override
    protected void onPause() {
        super.onPause();
        if (telemetry != null) telemetry.onPause();
    }

    /** Back is caught here, before SDL: it opens and closes the sidebar and the game never sees it. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (sidebar == null) return super.dispatchKeyEvent(e);
        int k = e.getKeyCode();
        if (setup != null && setup.isShowing()) { // the setup dialog owns Back and the controller; SDL and the sidebar get nothing
            if (k == KeyEvent.KEYCODE_BACK || k == KeyEvent.KEYCODE_BUTTON_B) return true;
            if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN) return super.dispatchKeyEvent(e);
            getWindow().superDispatchKeyEvent(e);
            return true;
        }
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0;
        if (k == KeyEvent.KEYCODE_BACK) {
            if (down) {
                if (sidebar.isOpen()) sidebar.back(); else sidebar.open();
            }
            return true;
        }
        if (sidebar.isOpen()) {
            if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN) return super.dispatchKeyEvent(e);
            if (k == KeyEvent.KEYCODE_BUTTON_B) { // controller B steps back like Back does
                if (down) sidebar.back();
                return true;
            }
            // the panel's views get the keys (D-pad moves focus, A presses); SDL gets none of them
            getWindow().superDispatchKeyEvent(e);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    /** Gesture or system Back that arrives without a key event. */
    @Override
    public void onBackPressed() {
        if (sidebar == null || (setup != null && setup.isShowing())) return;
        if (sidebar.isOpen()) sidebar.back(); else sidebar.open();
    }

    /** Called by the sidebar once it closes: the game's surface takes the keys back. */
    void sidebarClosed() {
        if (mSurface != null) mSurface.requestFocus();
    }

    private static final int REQ_IMPORT_DRIVER = 0x5D01;
    private static final int REQ_IMPORT_MOD = 0x5D02;
    private static final int REQ_IMPORT_COBALT = 0x5D03;

    /** Opens the system file picker for a mod archive (.zip or .7z). */
    void pickModArchive() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed",
                "application/x-7z-compressed", "application/octet-stream"});
        startActivityForResult(i, REQ_IMPORT_MOD);
    }

    /** Opens the system file picker for a Cobalt release.zip. */
    void pickCobaltZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        startActivityForResult(i, REQ_IMPORT_COBALT);
    }

    /** Opens the system file picker for a driver package (.zip with meta.json and the driver .so). */
    void pickDriverZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        startActivityForResult(i, REQ_IMPORT_DRIVER);
    }

    /** Opens the system file picker (several files for the key files). Results go to the setup dialog. */
    void pickFile(int req, String[] mimes, boolean multiple) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, mimes);
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, req);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        if (req >= SetupDialog.REQ_BASE && req <= SetupDialog.REQ_KEYS) {
            if (setup != null) setup.onPicked(req, res, data);
            return;
        }
        if (req == REQ_IMPORT_MOD) {
            if (res == RESULT_OK && data != null && data.getData() != null && sidebar != null)
                sidebar.importMod(data.getData());
            return;
        }
        if (req == REQ_IMPORT_COBALT) {
            if (res == RESULT_OK && data != null && data.getData() != null && sidebar != null)
                sidebar.importCobalt(data.getData());
            return;
        }
        if (req == REQ_IMPORT_DRIVER) {
            if (res == RESULT_OK && data != null && data.getData() != null && sidebar != null)
                sidebar.importDriver(data.getData());
            return;
        }
        super.onActivityResult(req, res, data);
    }

    void restartGame() {
        Intent i = new Intent(this, RestartActivity.class);
        i.putExtra("pid", Process.myPid());
        startActivity(i);
    }

    void quitGame() {
        finishAndRemoveTask();
        Process.killProcess(Process.myPid());
    }
}
