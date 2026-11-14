package dev.teton.somniel;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Updates Somniel from its newest GitHub release (github.com/teton-dev/somniel): finds the release's .apk, downloads it,
 * checks it (SHA-256 from the release notes when they list one, package name, a newer version code) and hands it to the
 * system's PackageInstaller. Android only installs it over this app when it is signed with the same key, so a tampered
 * download cannot replace the app. Nothing is downloaded without the user's yes; the check itself sends nothing but a
 * request for the public release list.
 */
final class AppUpdater {
    static final String REPO = "teton-dev/somniel";
    private static final String API = "https://api.github.com/repos/" + REPO + "/releases/latest";
    private static final String DOWNLOAD_PREFIX = "https://github.com/" + REPO + "/releases/download/";
    private static final long MAX_APK = 200L << 20;
    private static final String ACTION = "dev.teton.somniel.UPDATE_RESULT";

    interface Progress { void say(String message); }

    /** The newest release: its tag ("v0.1.5"), the APK to fetch, its size, the SHA-256 if the notes give one, the notes. */
    static final class Release {
        String tag, apkUrl, sha256, notes;
        long size;
    }

    private AppUpdater() {}

    /** Fetches the latest published (non-draft, non-pre-release) release. Blocking: call from a worker thread. */
    static Release fetchLatest() throws IOException {
        Release r = new Release();
        String whatsNewUrl = null;
        try {
            JSONObject rel = new JSONObject(httpString(API));
            r.tag = rel.getString("tag_name");
            r.notes = rel.optString("body", "");
            JSONArray assets = rel.getJSONArray("assets");
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                String name = a.getString("name");
                String url = a.getString("browser_download_url");
                if (name.endsWith(".apk") && r.apkUrl == null) {
                    r.apkUrl = url;
                    r.size = a.optLong("size", 0);
                } else if (name.equals("whatsnew.txt") && url.startsWith(DOWNLOAD_PREFIX) && a.optLong("size", 0) <= 16384) {
                    whatsNewUrl = url;
                }
            }
        } catch (org.json.JSONException e) {
            throw new IOException("unexpected answer from GitHub");
        }
        if (r.apkUrl == null || !r.apkUrl.startsWith(DOWNLOAD_PREFIX)) throw new IOException("the latest release has no APK");
        Matcher m = Pattern.compile("(?i)sha-?256\\W{0,6}([0-9a-f]{64})").matcher(r.notes);
        if (m.find()) r.sha256 = m.group(1).toLowerCase();
        if (whatsNewUrl != null) { // the short, hand-written summary shown in the update prompt; optional
            try {
                String s = httpString(whatsNewUrl).trim();
                if (!s.isEmpty()) r.notes = s;
            } catch (IOException ignored) { }
        }
        return r;
    }

    /** True when `tag` ("v0.1.5", "0.2") is a higher version than `current` ("0.1.4"). Unparseable tags are never newer. */
    static boolean isNewer(String tag, String current) {
        int[] a = parse(tag), b = parse(current);
        if (a == null || b == null) return false;
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? a[i] : 0, y = i < b.length ? b[i] : 0;
            if (x != y) return x > y;
        }
        return false;
    }

    private static int[] parse(String v) {
        Matcher m = Pattern.compile("(\\d+(?:\\.\\d+)*)").matcher(v == null ? "" : v);
        if (!m.find()) return null;
        String[] p = m.group(1).split("\\.");
        int[] out = new int[p.length];
        try {
            for (int i = 0; i < p.length; i++) out[i] = Integer.parseInt(p[i]);
        } catch (NumberFormatException e) {
            return null;
        }
        return out;
    }

    /** Downloads and checks the release's APK (a finished earlier download is reused). Returns the file. */
    static File download(Context ctx, Release r, Progress p) throws IOException {
        File dir = new File(ctx.getCacheDir(), "update");
        dir.mkdirs();
        File apk = new File(dir, "Somniel-" + r.tag.replaceAll("[^A-Za-z0-9._-]", "_") + ".apk");
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) if (!f.equals(apk)) f.delete();

        if (!(apk.isFile() && verifies(ctx, apk, r))) {
            File part = new File(dir, apk.getName() + ".part");
            HttpURLConnection c = open(r.apkUrl, "application/octet-stream");
            long total = c.getContentLengthLong();
            if (total > MAX_APK) { c.disconnect(); throw new IOException("download too large"); }
            long got = 0, lastUi = 0;
            byte[] buf = new byte[1 << 16];
            try (InputStream in = c.getInputStream(); OutputStream out = new java.io.FileOutputStream(part)) {
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    got += n;
                    if (got > MAX_APK) throw new IOException("download too large");
                    long now = System.currentTimeMillis();
                    if (now - lastUi > 300) {
                        lastUi = now;
                        p.say("Downloading " + r.tag + "... " + (total > 0 ? (got * 100 / total) + "%" : (got >> 10) + " KB"));
                    }
                }
            } catch (IOException e) {
                part.delete();
                throw e;
            } finally {
                c.disconnect();
            }
            apk.delete();
            if (!part.renameTo(apk)) throw new IOException("cannot store the download");
            if (!verifies(ctx, apk, r)) {
                apk.delete();
                throw new IOException("the download did not pass its checks (checksum, package or version)");
            }
        }
        return apk;
    }

    /** SHA-256 (when known), package name and a version code above the installed one. */
    private static boolean verifies(Context ctx, File apk, Release r) {
        try {
            if (r.sha256 != null && !r.sha256.equals(sha256(apk))) return false;
            PackageInfo info = ctx.getPackageManager().getPackageArchiveInfo(apk.getPath(), 0);
            if (info == null || !ctx.getPackageName().equals(info.packageName)) return false;
            PackageInfo cur = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            return info.getLongVersionCode() > cur.getLongVersionCode();
        } catch (Throwable t) {
            return false;
        }
    }

    private static String sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) md.update(buf, 0, n);
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest()) sb.append(String.format("%02x", b));
        return sb.toString();
    }

    /** Whether the user has allowed Somniel to install apps ("Install unknown apps"); the system asks for this once. */
    static boolean canInstall(Context ctx) {
        return ctx.getPackageManager().canRequestPackageInstalls();
    }

    /** Opens the system page where the user allows Somniel to install apps. */
    static void openInstallSettings(Context ctx) {
        Intent i = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + ctx.getPackageName()));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(i);
    }

    /**
     * Hands the APK to the PackageInstaller. The system may show its own confirmation (the first time, always); when the
     * install succeeds it replaces this process, so the app closes and the user starts it again. Blocking: worker thread.
     * `done` gets null on success of the hand-over, or the reason it failed.
     */
    static void install(final Context ctx, File apk, final java.util.function.Consumer<String> done) throws IOException {
        final PackageInstaller pi = ctx.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        sp.setAppPackageName(ctx.getPackageName());
        if (Build.VERSION.SDK_INT >= 31) sp.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        final int id = pi.createSession(sp);
        try (PackageInstaller.Session s = pi.openSession(id)) {
            try (OutputStream out = s.openWrite("somniel.apk", 0, apk.length());
                 InputStream in = new FileInputStream(apk)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                s.fsync(out);
            }
            final Context app = ctx.getApplicationContext();
            BroadcastReceiver rcv = new BroadcastReceiver() {
                @Override public void onReceive(Context c, Intent in) {
                    int status = in.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                    if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                        Intent confirm = in.getParcelableExtra(Intent.EXTRA_INTENT);
                        if (confirm != null) {
                            confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                            try { c.startActivity(confirm); } catch (Throwable t) { finish(c, this, "cannot show the install prompt"); }
                        }
                        return;
                    }
                    finish(c, this, status == PackageInstaller.STATUS_SUCCESS ? null
                            : String.valueOf(in.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)));
                }
                private void finish(Context c, BroadcastReceiver self, String err) {
                    try { c.unregisterReceiver(self); } catch (Throwable ignored) { }
                    done.accept(err);
                }
            };
            IntentFilter f = new IntentFilter(ACTION);
            if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(rcv, f, Context.RECEIVER_NOT_EXPORTED);
            else app.registerReceiver(rcv, f);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            PendingIntent pend = PendingIntent.getBroadcast(app, id,
                    new Intent(ACTION).setPackage(app.getPackageName()), flags);
            s.commit(pend.getIntentSender());
        } catch (IOException | RuntimeException e) {
            try { pi.abandonSession(id); } catch (Throwable ignored) { }
            throw e instanceof IOException ? (IOException) e : new IOException(e.getMessage());
        }
    }

    // ---- http ----

    private static HttpURLConnection open(String url, String accept) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setRequestProperty("User-Agent", "Somniel");
        c.setRequestProperty("Accept", accept);
        int code = c.getResponseCode();
        if (code != 200) {
            c.disconnect();
            throw new IOException(code == 403 || code == 429 ? "GitHub is rate limiting this network, try again later"
                    : "GitHub answered " + code);
        }
        return c;
    }

    private static String httpString(String url) throws IOException {
        HttpURLConnection c = open(url, "application/vnd.github+json");
        try (InputStream in = c.getInputStream()) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        } finally {
            c.disconnect();
        }
    }
}
