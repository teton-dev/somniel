package dev.teton.somniel;

import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;
import android.widget.CheckBox;
import android.widget.FrameLayout;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.UUID;

/**
 * Opt-in, anonymous performance and crash reporting, plus the first-boot warning for GPUs that are untested.
 *
 * Nothing is collected or sent until the user says yes (and never when the build has no server address in
 * res/values/telemetry.xml). What is sent: a random install id (reset-able), app version, device model, SoC and GPU
 * names, Android version, RAM size, frame-rate totals, and (after a crash) the faulting library+offset frames.
 * Never: names, accounts, IP addresses (the server does not store them), file paths, save data or game files.
 */
final class Telemetry {
    static native String nativeGpu();          // "name|driver|vendor|vkapi" once Vulkan is up, "" before
    static native double[] nativeTakePerf();   // see the V_* layout in telemetry.c

    // indices into nativeTakePerf() (keep in sync with the V_* enum in telemetry.c)
    private static final int P_SECS = 0, P_FRAMES = 1, P_MAX_GAP = 2, P_MIN_FPS = 3, P_WINDOWS = 4, P_OVER50 = 5,
            P_OVER100 = 6, P_OVER250 = 7, P_OVER1000 = 8, P_LONG_GAPS = 9, P_P50 = 10, P_P95 = 11, P_P99 = 12,
            P_P999 = 13, P_HITCHES = 14, P_CAUSE0 = 15 /* guest, cpu, gpu, pipe, tex, other */, P_PIPES = 21,
            P_PIPE_MS = 22, P_WIN_DROPPED = 23, P_TARGET_FPS = 24, P_COUNTED = 25, P_N = 26;
    private static final String[] CAUSES = {"guest", "cpu", "gpu", "pipe", "tex", "other"};

    private static final long PERF_EVERY_MS = 5 * 60 * 1000L;
    private static final int QUEUE_MAX = 20;

    private final SomnielActivity act;
    private final SharedPreferences prefs;
    private final String url, key;
    private final File queue;
    private final HandlerThread thread = new HandlerThread("somniel-telemetry");
    private Handler handler;
    private boolean running;
    private String glRenderer = "";
    private volatile int thermalMax;       // highest PowerManager thermal status since the last report (0 none .. 6 shutdown)
    private boolean thermalListening;

    Telemetry(SomnielActivity act) {
        this.act = act;
        this.prefs = act.getSharedPreferences("telemetry", Context.MODE_PRIVATE);
        this.url = res("telemetry_url");
        this.key = res("telemetry_key");
        this.queue = new File(act.getFilesDir(), "telemetry_queue");
    }

    private String res(String name) {
        int id = act.getResources().getIdentifier(name, "string", act.getPackageName());
        return id == 0 ? "" : act.getString(id).trim();
    }

    /** False in a build without a server address: no prompt, no toggle, no code path that can send anything. */
    boolean available() { return !url.isEmpty(); }

    /** null = not asked yet. */
    Boolean consent() { return prefs.contains("consent") ? prefs.getBoolean("consent", false) : null; }

    boolean enabled() { return available() && Boolean.TRUE.equals(consent()); }

    void setConsent(boolean yes) {
        prefs.edit().putBoolean("consent", yes).commit();
        if (yes) start();
        else {
            stop();
            deleteQueue();
            new File(act.getFilesDir(), "crash_report.txt").delete();
            nativeTakePerf(); // drop what was counted so far
        }
    }

    /** A new random id (the old one is forgotten: the server can no longer link new data to the old). */
    void resetId() { prefs.edit().remove("install_id").commit(); }

    private String installId() {
        String id = prefs.getString("install_id", null);
        if (id == null) {
            id = UUID.randomUUID().toString();
            prefs.edit().putString("install_id", id).commit();
        }
        return id;
    }

    // ---- GPU warning ----

    /** GL_RENDERER of the device's GPU ("Adreno (TM) 740", "Mali-G715", "Samsung Xclipse 940"), "" if unknown. */
    String gpuName() {
        if (!glRenderer.isEmpty()) return glRenderer;
        EGLDisplay dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        EGLContext ctx = EGL14.EGL_NO_CONTEXT;
        EGLSurface surf = EGL14.EGL_NO_SURFACE;
        try {
            int[] ver = new int[2];
            if (!EGL14.eglInitialize(dpy, ver, 0, ver, 1)) return "";
            EGLConfig[] cfg = new EGLConfig[1];
            int[] n = new int[1];
            int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                    EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE};
            if (!EGL14.eglChooseConfig(dpy, attrs, 0, cfg, 0, 1, n, 0) || n[0] < 1) return "";
            ctx = EGL14.eglCreateContext(dpy, cfg[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            surf = EGL14.eglCreatePbufferSurface(dpy, cfg[0], new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1,
                    EGL14.EGL_NONE}, 0);
            if (!EGL14.eglMakeCurrent(dpy, surf, surf, ctx)) return "";
            String r = GLES20.glGetString(GLES20.GL_RENDERER);
            glRenderer = r == null ? "" : r;
        } catch (Throwable t) {
            // leave it unknown
        } finally {
            try {
                EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                if (surf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, surf);
                if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx);
            } catch (Throwable ignored) {
            }
        }
        return glRenderer;
    }

    /** Mali and Xclipse are not tested with Somniel (it was built and verified on Adreno 740). */
    boolean untestedGpu() {
        String g = gpuName().toLowerCase();
        if (g.contains("mali") || g.contains("xclipse")) return true;
        if (!g.isEmpty()) return false;
        // EGL failed: fall back to the SoC maker (anything that is not Qualcomm is treated as untested)
        String soc = (Build.SOC_MANUFACTURER + " " + Build.HARDWARE).toLowerCase();
        return !(soc.contains("qualcomm") || soc.contains("qcom"));
    }

    // ---- dialogs ----

    void showGpuWarning(Runnable then) {
        if (prefs.getBoolean("gpu_warning_hidden", false)) { // "Don't show again" was ticked
            if (then != null) then.run();
            return;
        }
        String gpu = gpuName().isEmpty() ? "non-Adreno GPU" : gpuName();
        boolean mali = gpuName().toLowerCase().contains("mali");
        CheckBox hide = new CheckBox(act);
        hide.setText("Don't show again");
        int pad = Math.round(20 * act.getResources().getDisplayMetrics().density);
        FrameLayout box = new FrameLayout(act);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(hide);
        AlertDialog d = new AlertDialog.Builder(act)
                .setTitle(mali ? "Mali support is in early stages" : "Your GPU is untested")
                .setMessage(mali
                        ? "This device has a " + gpu + ".\n\n"
                        + "Mali GPU support in Somniel is at an early stage and is likely to be broken: expect wrong "
                        + "graphics, slowdowns, crashes or freezes.\n\n"
                        + "Lower settings are in the sidebar under Graphics. Reports help us fix it."
                        : "This device has a " + gpu + ".\n\n"
                        + "Somniel has only been tested on Qualcomm Adreno GPUs. Xclipse and other GPUs are untested and "
                        + "unverified: the game may show wrong graphics, run slowly, or not start at all.\n\n"
                        + "You can still try it. Reports from the in-game sidebar help us fix it.")
                .setView(box)
                .setCancelable(false)
                .setPositiveButton("I understand", (di, w) -> {
                    if (hide.isChecked()) prefs.edit().putBoolean("gpu_warning_hidden", true).commit();
                    if (then != null) then.run();
                })
                .create();
        d.show();
    }

    void showConsent(Runnable then) {
        AlertDialog d = new AlertDialog.Builder(act)
                .setTitle("Share anonymous data?")
                .setMessage("Help improve Somniel by sharing anonymous performance and crash data.\n\n"
                        + "Shared: device model, chipset and GPU names, Android version, RAM, frame-rate and frame-time "
                        + "totals (including a rough reason for slow frames and the device's heat-throttling state), "
                        + "and crash locations (library and offset only), tied to a random ID.\n\n"
                        + "Never shared: your name, accounts, IP address, files, saves or game data.\n\n"
                        + "You can change this any time in the sidebar.")
                .setCancelable(false)
                .setPositiveButton("Share", (di, w) -> { setConsent(true); if (then != null) then.run(); })
                .setNegativeButton("No thanks", (di, w) -> { setConsent(false); if (then != null) then.run(); })
                .create();
        d.show();
    }

    // ---- collection and upload ----

    /** Starts the periodic upload (only with consent). Sends a pending crash report straight away. */
    synchronized void start() {
        if (!enabled() || running) return;
        running = true;
        if (!thread.isAlive()) thread.start();
        if (handler == null) handler = new Handler(thread.getLooper());
        handler.postDelayed(this::tick, 45_000); // the GPU name is known by then; the first window has data
        handler.post(this::sendPending);
        listenThermal();
    }

    /** Throttling is the usual reason for a device that was fine a minute ago: remember the worst status per report. */
    private void listenThermal() {
        if (thermalListening) return;
        try {
            PowerManager pm = (PowerManager) act.getSystemService(Context.POWER_SERVICE);
            if (pm == null) return;
            thermalMax = pm.getCurrentThermalStatus();
            pm.addThermalStatusListener(r -> handler.post(r), s -> { if (s > thermalMax) thermalMax = s; });
            thermalListening = true;
        } catch (Throwable ignored) {
        }
    }

    private int takeThermalMax() {
        int m = thermalMax;
        try {
            PowerManager pm = (PowerManager) act.getSystemService(Context.POWER_SERVICE);
            thermalMax = pm == null ? 0 : pm.getCurrentThermalStatus();
        } catch (Throwable t) {
            thermalMax = 0;
        }
        return m;
    }

    private synchronized void stop() {
        running = false;
        if (handler != null) handler.removeCallbacksAndMessages(null);
    }

    private void tick() {
        if (!running) return;
        collect(false);
        sendPending();
        handler.postDelayed(this::tick, PERF_EVERY_MS);
    }

    /** The app goes to the background (it may be killed there): write what is counted so far, send in the background. */
    void onPause() {
        if (!enabled() || !running) return;
        collect(false);
        handler.post(this::sendPending);
    }

    /** Takes the frame-rate totals since the last call and queues them as one report. */
    private void collect(boolean unused) {
        try {
            double[] p = nativeTakePerf();
            if (p == null || p.length < P_N || p[P_WINDOWS] < 1 || p[P_SECS] < 10) return; // under 10 s of presented frames is not a sample
            JSONObject perf = new JSONObject();
            perf.put("seconds", Math.round(p[P_SECS]));
            perf.put("fps_avg", Math.round(p[P_FRAMES] / p[P_SECS] * 10) / 10.0);
            perf.put("fps_low_window", Math.round(p[P_MIN_FPS] * 10) / 10.0);
            perf.put("longest_frame_ms", Math.round(p[P_MAX_GAP]));
            perf.put("over_50ms", (int) p[P_OVER50]);
            perf.put("over_100ms", (int) p[P_OVER100]);
            perf.put("over_250ms", (int) p[P_OVER250]);
            perf.put("over_1000ms", (int) p[P_OVER1000]);
            perf.put("long_gaps", (int) p[P_LONG_GAPS]);   // gaps over 5 s between frames: freezes, not counted in the rest
            perf.put("p50_ms", (int) p[P_P50]);
            perf.put("p95_ms", (int) p[P_P95]);
            perf.put("p99_ms", (int) p[P_P99]);
            perf.put("p999_ms", (int) p[P_P999]);
            perf.put("target_fps", (int) p[P_TARGET_FPS]); // 0 = follows the game
            perf.put("hitches", (int) p[P_HITCHES]);       // frames over 1.5 target periods (25 ms at 60, 50 ms at 30)
            for (int k = 0; k < CAUSES.length; k++) perf.put("hitch_" + CAUSES[k], (int) p[P_CAUSE0 + k]);
            perf.put("pipes_built", (int) p[P_PIPES]);
            perf.put("pipe_ms", Math.round(p[P_PIPE_MS]));
            perf.put("windows_dropped", (int) p[P_WIN_DROPPED]); // 5 s windows thrown away for touching the background
            perf.put("thermal_max", takeThermalMax());
            perf.put("up_s", Math.max(0, (SystemClock.elapsedRealtime() - Process.getStartElapsedRealtime()) / 1000));
            JSONObject o = base("perf");
            o.put("perf", perf);
            enqueue(o);
        } catch (Throwable ignored) {
        }
    }

    private JSONObject base(String type) throws Exception {
        JSONObject o = new JSONObject();
        o.put("v", 1);
        o.put("type", type);
        o.put("install", installId());
        o.put("rid", UUID.randomUUID().toString().substring(0, 18)); // the server drops a re-sent copy of the same report
        o.put("ts", System.currentTimeMillis() / 1000);
        o.put("app", BuildVersion.name(act));
        JSONObject dev = new JSONObject();
        dev.put("model", Build.MANUFACTURER + " " + Build.MODEL);
        dev.put("soc", (Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL).trim());
        dev.put("hardware", Build.HARDWARE);
        dev.put("android", Build.VERSION.RELEASE);
        dev.put("sdk", Build.VERSION.SDK_INT);
        dev.put("gl_renderer", gpuName());
        String vk = nativeGpu();
        String[] f = vk.split("\\|");
        if (f.length == 4) {
            dev.put("vk_name", f[0]);
            dev.put("vk_driver", f[1]);
            dev.put("vk_vendor", f[2]);
            dev.put("vk_api", f[3]);
        }
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        ((ActivityManager) act.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(mi);
        dev.put("ram_gb", Math.round(mi.totalMem / 1073741824.0));
        o.put("device", dev);
        return o;
    }

    private synchronized void enqueue(JSONObject o) throws Exception {
        if (!queue.isDirectory() && !queue.mkdirs()) return;
        File[] old = queue.listFiles();
        if (old != null && old.length >= QUEUE_MAX) { // offline for a long time: drop the oldest
            Arrays.sort(old);
            for (int i = 0; i <= old.length - QUEUE_MAX; i++) old[i].delete();
        }
        File f = new File(queue, System.currentTimeMillis() + "_" + (int) (Math.random() * 1000) + ".json");
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(o.toString().getBytes(StandardCharsets.UTF_8));
        }
    }

    private void deleteQueue() {
        File[] fs = queue.listFiles();
        if (fs != null) for (File f : fs) f.delete();
    }

    /** Turns a crash_report.txt left by the previous run into a queued report, then uploads the queue. */
    private void sendPending() {
        if (!running) return;
        try {
            File cr = new File(act.getFilesDir(), "crash_report.txt");
            if (cr.isFile() && cr.length() < 4096) {
                byte[] b = new byte[(int) cr.length()];
                try (java.io.FileInputStream in = new java.io.FileInputStream(cr)) { in.read(b); }
                JSONObject crash = new JSONObject();
                for (String line : new String(b, StandardCharsets.UTF_8).split("\n")) {
                    int eq = line.indexOf('=');
                    if (eq > 0) crash.put(line.substring(0, eq), line.substring(eq + 1));
                }
                JSONObject o = base("crash");
                o.put("crash", crash);
                enqueue(o);
            }
            cr.delete();
        } catch (Throwable ignored) {
        }
        File[] fs = queue.listFiles();
        if (fs == null) return;
        Arrays.sort(fs);
        for (File f : fs) {
            int code = post(f);
            if (code < 0) return;                  // network trouble: keep everything, try later
            f.delete();                            // delivered, or refused for good (4xx): do not retry forever
        }
    }

    /** HTTP status, or -1 for a network error or a server error (retry later). */
    private int post(File f) {
        HttpURLConnection c = null;
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
                byte[] buf = new byte[4096];
                for (int n; (n = in.read(buf)) > 0; ) bo.write(buf, 0, n);
            }
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(8000);
            c.setReadTimeout(8000);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.setRequestProperty("X-Somniel-Key", key);
            try (OutputStream out = c.getOutputStream()) { out.write(bo.toByteArray()); }
            int code = c.getResponseCode();
            try (InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream()) {
                if (in != null) while (in.read() >= 0) { /* drain */ }
            }
            return code >= 500 ? -1 : code;
        } catch (Throwable t) {
            return -1;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
