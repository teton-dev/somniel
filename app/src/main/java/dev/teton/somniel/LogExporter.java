package dev.teton.somniel;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Copies somniel.log into the shared Download/somniel_logs folder so a
 * user can send it without adb. MediaStore creates the folder when it is missing and needs no storage permission
 * (Android 10+). The native side calls {@link #exportLog} (som_export_log in debug_log.c) when something went wrong:
 * audio could not start, the renderer stalled. {@link #exportPreviousCrash} runs at launch for a crash of the last run.
 */
final class LogExporter {
    static final String DIR = Environment.DIRECTORY_DOWNLOADS + "/somniel_logs";

    private LogExporter() {}

    /** Called from native code on a background thread: saves the log, then shows a toast with where it went. */
    static void exportLog(final Activity act, final String srcPath, final String prefix, final String reason) {
        String msg;
        try {
            String name = save(act, srcPath, prefix, reason);
            msg = "A diagnostic log was saved to " + DIR + "/" + name + ": please send it with your report.";
        } catch (Exception e) {
            msg = "Saving a diagnostic log failed: " + e.getMessage();
        }
        android.util.Log.i("Somniel", msg);
        final String m = msg;
        act.runOnUiThread(() -> Toast.makeText(act, m, Toast.LENGTH_LONG).show());
    }

    /** The log of the current (or, before the native side starts, the previous) run. */
    static String logPath(Context ctx) {
        File ext = ctx.getExternalFilesDir(null);
        if (ext != null && new File(ext, "log_external").exists()) return new File(ext, "somniel.log").getPath();
        return new File(ctx.getFilesDir(), "somniel.log").getPath();
    }

    /**
     * At launch, before the native side truncates somniel.log: if the last run ended in a fault (crash_report.txt from
     * the crash handler, or fault_stall.txt from the renderer watchdog; not a plain swipe-away), copy that run's log to
     * filesDir/fault_log.txt and return the reason, else null. The user is then asked whether to save it.
     */
    static String stashPreviousFault(Context ctx) {
        try {
            File ext = ctx.getExternalFilesDir(null);
            StringBuilder sb = new StringBuilder();
            long newest = 0;
            for (String n : new String[] {"crash_report.txt", "fault_stall.txt"}) {
                File f = new File(ctx.getFilesDir(), n);
                if (!f.isFile() && ext != null) f = new File(ext, n);
                if (!f.isFile()) continue;
                newest = Math.max(newest, f.lastModified());
                byte[] b = new byte[(int) Math.min(f.length(), 8192)];
                try (InputStream is = new FileInputStream(f)) { int r = is.read(b); if (r > 0) sb.append(n).append(":\n").append(new String(b, 0, r, StandardCharsets.UTF_8)).append('\n'); }
                if (n.startsWith("fault")) f.delete(); // ours only; telemetry owns crash_report.txt
            }
            if (sb.length() == 0) return null;
            // telemetry deletes the crash report only when it is on: ask once per report
            File mark = new File(ctx.getFilesDir(), "fault_asked");
            String stamp = Long.toString(newest);
            if (mark.isFile() && new String(java.nio.file.Files.readAllBytes(mark.toPath()), StandardCharsets.UTF_8).equals(stamp)) return null;
            java.nio.file.Files.write(mark.toPath(), stamp.getBytes(StandardCharsets.UTF_8));
            File log = new File(logPath(ctx));
            if (log.isFile()) java.nio.file.Files.copy(log.toPath(), new File(ctx.getFilesDir(), "fault_log.txt").toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            return "The previous run ended in a fault.\n" + sb;
        } catch (Exception e) {
            android.util.Log.w("Somniel", "fault stash failed: " + e);
            return null;
        }
    }

    /** Asks whether to save the stashed log of the previous fault to Download/somniel_logs. */
    static void askToSave(final Activity act, final String reason) {
        new android.app.AlertDialog.Builder(act)
                .setTitle("Somniel closed unexpectedly")
                .setMessage("Last time the game crashed or stopped responding. Save its log to " + DIR + " so you can send it with a bug report?")
                .setPositiveButton("Save log", (d, w) -> new Thread(() -> {
                    String msg;
                    try {
                        msg = "Log saved to " + DIR + "/" + save(act, new File(act.getFilesDir(), "fault_log.txt").getPath(), "somniel_crash", reason);
                    } catch (Exception e) { msg = "Saving the log failed: " + e.getMessage(); }
                    final String m = msg;
                    act.runOnUiThread(() -> Toast.makeText(act, m, Toast.LENGTH_LONG).show());
                }).start())
                .setNegativeButton("No thanks", null)
                .show();
    }

    /** Writes a header (device, audio outputs, reason) plus the file at srcPath to Download/Somniel_log; returns the name. */
    static String save(Context ctx, String srcPath, String prefix, String reason) throws IOException {
        String name = prefix + "_" + new SimpleDateFormat("yyyy-MM-dd_HHmmss", Locale.US).format(new Date()) + ".txt";
        ContentResolver cr = ctx.getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        v.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, DIR);
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = cr.insert(MediaStore.Files.getContentUri("external"), v); // Documents/ is not a Downloads-collection path
        if (uri == null) throw new IOException("could not create " + DIR + "/" + name);
        try {
            try (OutputStream os = cr.openOutputStream(uri, "w")) {
                if (os == null) throw new IOException("could not write " + name);
                os.write(header(ctx, reason).getBytes(StandardCharsets.UTF_8));
                File src = new File(srcPath);
                if (src.isFile()) {
                    try (InputStream is = new FileInputStream(src)) {
                        byte[] buf = new byte[1 << 16];
                        for (int r; (r = is.read(buf)) > 0; ) os.write(buf, 0, r);
                    }
                } else {
                    os.write(("(no log at " + srcPath + ")\n").getBytes(StandardCharsets.UTF_8));
                }
            }
            ContentValues done = new ContentValues();
            done.put(MediaStore.MediaColumns.IS_PENDING, 0);
            cr.update(uri, done, null, null);
            return name;
        } catch (IOException | RuntimeException e) {
            try { cr.delete(uri, null, null); } catch (Exception ignored) {}
            throw e;
        }
    }

    private static String header(Context ctx, String reason) {
        StringBuilder sb = new StringBuilder();
        sb.append("Somniel log export\n").append(reason).append('\n');
        try {
            sb.append("App: ").append(ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName).append('\n');
        } catch (Exception ignored) {}
        sb.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
          .append(" (").append(Build.HARDWARE).append(")\n");
        if (Build.VERSION.SDK_INT >= 31) sb.append("SoC: ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        sb.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am != null) {
            sb.append("Audio: output rate ").append(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE))
              .append(", frames/buffer ").append(am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER))
              .append(", music volume ").append(am.getStreamVolume(AudioManager.STREAM_MUSIC)).append('/')
              .append(am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)).append(", mode ").append(am.getMode()).append('\n');
            for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
                sb.append("  output id ").append(d.getId()).append(" type ").append(d.getType()).append(" \"")
                  .append(d.getProductName()).append("\"\n");
        }
        sb.append("----\n");
        return sb.toString();
    }
}
