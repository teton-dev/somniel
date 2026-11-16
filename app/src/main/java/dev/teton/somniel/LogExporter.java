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
 * Copies somniel.log into the shared Download/Somniel_log folder so a user can send it without adb. MediaStore creates
 * the folder when it is missing and needs no storage permission (Android 10+). The native side calls
 * {@link #saveAudioLog} when the audio output could not start (services_audio.c).
 */
final class LogExporter {
    static final String DIR = Environment.DIRECTORY_DOWNLOADS + "/Somniel_log";

    private LogExporter() {}

    /** Called from native code on a background thread: saves the log, then shows a toast with where it went. */
    static void saveAudioLog(final Activity act, final String srcPath, final String reason) {
        String msg;
        try {
            String name = save(act, srcPath, "somniel_audio", "Audio failure: " + reason);
            msg = "Audio could not start. A log was saved to " + DIR + "/" + name + ": please send it with your report.";
        } catch (Exception e) {
            msg = "Audio could not start (saving a log failed: " + e.getMessage() + ")";
        }
        android.util.Log.i("Somniel", msg);
        final String m = msg;
        act.runOnUiThread(() -> Toast.makeText(act, m, Toast.LENGTH_LONG).show());
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
        Uri uri = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
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
