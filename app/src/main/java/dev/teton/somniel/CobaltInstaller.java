package dev.teton.somniel;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Installs the Cobalt mod loader (github.com/Raytwo/Cobalt, by Raytwo) that the user fetches themselves: Somniel does not
 * ship it (the project has no licence that allows redistribution). Two ways in: download the latest GitHub release, or
 * import a release.zip the user already has. Either way the three files the native side reads land in the SD layout:
 *   <data>/sd/atmosphere/contents/0100A6301214E000/exefs/{subsdk9,main.npdm} and <data>/sd/engage/cobalt/cache.lut
 * and sd/engage/cobalt/.somniel_bundled records the version (the tag, or "imported"). Cobalt is read when the game
 * starts, so a restart follows an install.
 */
final class CobaltInstaller {
    static final String REPO = "Raytwo/Cobalt";
    private static final String API = "https://api.github.com/repos/" + REPO + "/releases/latest";
    private static final String DOWNLOAD_PREFIX = "https://github.com/" + REPO + "/releases/download/";
    private static final String EXEFS = "sd/atmosphere/contents/0100A6301214E000/exefs";
    private static final String COBALT = "sd/engage/cobalt";
    private static final String MARKER = ".somniel_bundled";
    private static final long MAX_ZIP = 64L << 20;

    interface Progress { void say(String message); }

    private CobaltInstaller() {}

    private static File ext(Context c) throws IOException {
        File e = c.getExternalFilesDir(null);
        if (e == null) throw new IOException("storage unavailable");
        return e;
    }

    static boolean installed(Context ctx) {
        File e = ctx.getExternalFilesDir(null);
        return e != null && new File(e, EXEFS + "/subsdk9").isFile();
    }

    /** "v1.31.0" when Somniel installed it, "installed" for a copy put there by hand, null when absent. */
    static String version(Context ctx) {
        File e = ctx.getExternalFilesDir(null);
        if (e == null || !new File(e, EXEFS + "/subsdk9").isFile()) return null;
        File m = new File(e, COBALT + "/" + MARKER);
        try {
            if (m.isFile()) {
                String v = readFile(m).trim();
                if (!v.isEmpty() && !v.equals("imported")) return v;
            }
        } catch (IOException ignored) { }
        return "installed";
    }

    /** Fetches the newest release.zip from GitHub and installs it. Blocking: call from a worker thread. Returns the tag. */
    static String downloadLatest(Context ctx, Progress p) throws IOException {
        p.say("Checking the latest Cobalt release...");
        String tag, url;
        try {
            JSONObject rel = new JSONObject(httpString(API));
            tag = rel.getString("tag_name");
            JSONArray assets = rel.getJSONArray("assets");
            url = null;
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                String n = a.getString("name");
                if (n.equals("release.zip") || (url == null && n.endsWith(".zip"))) url = a.getString("browser_download_url");
            }
        } catch (org.json.JSONException e) {
            throw new IOException("unexpected answer from GitHub");
        }
        if (url == null || !url.startsWith(DOWNLOAD_PREFIX)) throw new IOException("the latest release has no zip to download");

        File tmp = new File(ctx.getCacheDir(), "cobalt-download.zip");
        try {
            HttpURLConnection c = open(url);
            long total = c.getContentLengthLong();
            if (total > MAX_ZIP) throw new IOException("download too large");
            long got = 0;
            long lastUi = 0;
            byte[] buf = new byte[1 << 16];
            try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(tmp)) {
                for (int r; (r = in.read(buf)) > 0; ) {
                    out.write(buf, 0, r);
                    got += r;
                    if (got > MAX_ZIP) throw new IOException("download too large");
                    long now = System.currentTimeMillis();
                    if (now - lastUi > 300) {
                        lastUi = now;
                        p.say("Downloading Cobalt " + tag + "... " + (total > 0 ? (got * 100 / total) + "%" : (got >> 10) + " KB"));
                    }
                }
            } finally {
                c.disconnect();
            }
            p.say("Installing Cobalt " + tag + "...");
            try (InputStream in = new java.io.FileInputStream(tmp)) {
                installZip(ctx, in, tag);
            }
        } finally {
            tmp.delete();
        }
        return tag;
    }

    /**
     * Installs the Cobalt files found in a release zip (any wrapper folder is fine): subsdk9 and main.npdm from
     * .../0100A6301214E000/exefs/ and cache.lut. Nothing is touched unless all three are present.
     */
    static void installZip(Context ctx, InputStream zip, String version) throws IOException {
        File root = ext(ctx);
        File stage = new File(root, "sd/.cobalt_stage");
        deleteTree(stage);
        if (!stage.mkdirs()) throw new IOException("cannot create " + stage);
        try {
            boolean sub = false, npdm = false, lut = false;
            try (ZipInputStream z = new ZipInputStream(zip)) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    if (e.isDirectory()) continue;
                    String n = e.getName().replace('\\', '/');
                    String dst = null;
                    if (n.endsWith("0100A6301214E000/exefs/subsdk9")) { dst = "subsdk9"; sub = true; }
                    else if (n.endsWith("0100A6301214E000/exefs/main.npdm")) { dst = "main.npdm"; npdm = true; }
                    else if (n.endsWith("cobalt/cache.lut")) { dst = "cache.lut"; lut = true; }
                    if (dst == null) continue;
                    try (OutputStream out = new FileOutputStream(new File(stage, dst))) {
                        byte[] buf = new byte[1 << 16];
                        long total = 0;
                        for (int r; (r = z.read(buf)) > 0; ) {
                            total += r;
                            if (total > MAX_ZIP) throw new IOException("file too large in archive");
                            out.write(buf, 0, r);
                        }
                    }
                }
            }
            if (!(sub && npdm && lut)) throw new IOException("this zip is not a Cobalt release (subsdk9, main.npdm or cache.lut missing)");
            move(new File(stage, "subsdk9"), new File(root, EXEFS + "/subsdk9"));
            move(new File(stage, "main.npdm"), new File(root, EXEFS + "/main.npdm"));
            move(new File(stage, "cache.lut"), new File(root, COBALT + "/cache.lut"));
            new File(root, "sd/engage/mods").mkdirs();
            writeFile(new File(root, COBALT + "/" + MARKER), version);
        } finally {
            deleteTree(stage);
        }
    }

    // ---- helpers ----

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setRequestProperty("User-Agent", "Somniel");
        c.setRequestProperty("Accept", "application/vnd.github+json");
        int code = c.getResponseCode();
        if (code != 200) {
            c.disconnect();
            throw new IOException(code == 403 || code == 429 ? "GitHub is rate limiting this network, try again later"
                    : "GitHub answered " + code);
        }
        return c;
    }

    private static String httpString(String url) throws IOException {
        HttpURLConnection c = open(url);
        try (InputStream in = c.getInputStream()) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        } finally {
            c.disconnect();
        }
    }

    private static void move(File src, File dst) throws IOException {
        File dir = dst.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        if (!src.renameTo(dst)) {
            dst.delete();
            if (!src.renameTo(dst)) throw new IOException("cannot replace " + dst);
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private static String readFile(File f) throws IOException {
        try (InputStream in = new java.io.FileInputStream(f)) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    private static void writeFile(File f, String s) throws IOException {
        File dir = f.getParentFile();
        if (dir != null) dir.mkdirs();
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes(StandardCharsets.UTF_8));
        }
    }
}
