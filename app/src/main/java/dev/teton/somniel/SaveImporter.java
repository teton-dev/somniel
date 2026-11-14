package dev.teton.somniel;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.provider.MediaStore;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Save import. The user picks a .zip or the save files themselves (Global, Auto, Manual0...); they are checked and
 * copied to <data>/save_import, and the game restarts. At the next start, before the game boots (apply(), from
 * SomnielActivity.onCreate), the current saves are zipped to Documents/Engage-Backup/ (a public folder the Files app
 * shows), that zip is read back to check it, the account save folder is emptied, and the staged files are moved in.
 * Nothing is removed unless the backup checked out.
 */
final class SaveImporter {
    /** The account save the game uses: type 1, user g_local (hooks_system.c), see services_hostfs.c save_dir_for. */
    static final String ACCOUNT_SAVE = "t1_5eed0002b0b000025eed0001a11ce001_0_0";
    static final String BACKUP_DIR = Environment.DIRECTORY_DOCUMENTS + "/Engage-Backup";
    private static final Pattern KNOWN = Pattern.compile("Global|Auto|Manual\\d+");
    private static final long MAX_TOTAL = 256L << 20;
    private static final int MAX_FILES = 512;
    private static final String READY = ".ready";

    private SaveImporter() {}

    static File saveRoot(Context c) { return new File(c.getExternalFilesDir(null), "save"); }
    static File accountDir(Context c) { return new File(saveRoot(c), ACCOUNT_SAVE); }
    static File staging(Context c) { return new File(c.getExternalFilesDir(null), "save_import"); }

    /** What stage() found: the file names that will replace the current saves. */
    static final class Staged {
        final List<String> names = new ArrayList<>();
        long bytes;
    }

    /**
     * Copies the picked saves to the staging folder. One picked file that is a zip: the folder in it holding the most
     * save files (Global, Auto, ManualN) is used, with every file in that folder (any wrapper folders are fine; a
     * Ryujinx-style 0/ and 1/ pair picks 0/). Otherwise the picked files are the save files. Blocking.
     */
    static Staged stage(Context ctx, List<Uri> uris) throws IOException {
        File st = staging(ctx), tmp = new File(ctx.getExternalFilesDir(null), "save_import.tmp");
        deleteTree(st);
        deleteTree(tmp);
        try {
            if (!tmp.mkdirs()) throw new IOException("could not create a work folder");
            ContentResolver cr = ctx.getContentResolver();
            Map<String, List<File>> byFolder = new HashMap<>();
            long[] total = {0};
            if (uris.size() == 1 && isZip(cr, uris.get(0))) {
                extractZip(cr, uris.get(0), tmp, byFolder, total);
            } else {
                List<File> files = new ArrayList<>();
                for (Uri u : uris) {
                    String n = safeName(displayName(ctx, u));
                    if (n == null) continue;
                    File out = new File(tmp, n);
                    try (InputStream in = cr.openInputStream(u)) {
                        if (in == null) throw new IOException("could not open " + n);
                        copy(in, out, total);
                    }
                    files.add(out);
                }
                byFolder.put("", files);
            }
            // the folder with the most save files; ties: the shallower one, then the first by name
            String best = null;
            int bestScore = 0;
            for (Map.Entry<String, List<File>> e : byFolder.entrySet()) {
                int score = 0;
                for (File f : e.getValue()) if (KNOWN.matcher(f.getName()).matches()) score++;
                String k = e.getKey();
                if (score > bestScore || (score == bestScore && score > 0 && (depth(k) < depth(best)
                        || (depth(k) == depth(best) && k.compareTo(best) < 0)))) {
                    best = k;
                    bestScore = score;
                }
            }
            if (best == null)
                throw new IOException("no Fire Emblem Engage save files found (they are named Global, Auto, Manual0, Manual1...)");
            if (!st.mkdirs()) throw new IOException("could not create the import folder");
            Staged s = new Staged();
            for (File f : byFolder.get(best)) {
                if (!f.renameTo(new File(st, f.getName()))) throw new IOException("could not stage " + f.getName());
                s.names.add(f.getName());
                s.bytes += new File(st, f.getName()).length();
            }
            java.util.Collections.sort(s.names);
            return s;
        } catch (IOException | RuntimeException e) {
            deleteTree(st);
            throw e;
        } finally {
            deleteTree(tmp);
        }
    }

    /** Marks the staged saves to be applied at the next start (then the caller restarts the game). */
    static void commit(Context ctx) throws IOException {
        if (!new File(staging(ctx), READY).createNewFile() && !new File(staging(ctx), READY).exists())
            throw new IOException("could not mark the import");
    }

    static void discard(Context ctx) { deleteTree(staging(ctx)); }

    /**
     * At start, before the game runs: applies a committed import. Returns a message for the user, or null when there
     * was nothing to do. A leftover uncommitted import is dropped.
     */
    static String apply(Context ctx) {
        File st = staging(ctx);
        if (!st.isDirectory()) return null;
        if (!new File(st, READY).isFile()) { deleteTree(st); return null; }
        String backup = null;
        boolean removed = false;
        try {
            backup = backUp(ctx);
            File acct = accountDir(ctx);
            deleteTree(acct);
            removed = true;
            if (!acct.mkdirs()) throw new IOException("could not create the save folder");
            int n = 0;
            File[] in = st.listFiles();
            if (in != null)
                for (File f : in) {
                    if (f.getName().equals(READY)) continue;
                    if (!f.renameTo(new File(acct, f.getName()))) throw new IOException("could not move " + f.getName());
                    n++;
                }
            deleteTree(st);
            som("saves imported: " + n + " files, backup " + backup);
            return "Imported " + n + " save file" + (n == 1 ? "" : "s") + ".\n\n" + (backup != null
                    ? "Your previous saves were backed up to " + BACKUP_DIR + "/" + backup + " in this device's storage."
                    : "There were no previous saves to back up.");
        } catch (Throwable e) {
            deleteTree(st);
            som("save import failed: " + e);
            String why = e.getMessage() != null ? e.getMessage() : e.toString();
            if (!removed) return "The save import failed: " + why + ".\n\nYour saves were not changed.";
            return "The save import failed part-way: " + why + ".\n\nYour previous saves are in " + BACKUP_DIR + "/"
                    + backup + "; import that zip to put them back.";
        }
    }

    /**
     * Zips everything under <data>/save to Documents/Engage-Backup/Engage-saves-<time>.zip and reads it back to check
     * every file arrived whole. Returns the zip's name, or null when there are no saves. Throws if anything is off.
     */
    private static String backUp(Context ctx) throws IOException {
        File root = saveRoot(ctx);
        List<File> files = new ArrayList<>();
        listFiles(root, files);
        if (files.isEmpty()) return null;
        String name = "Engage-saves-" + new SimpleDateFormat("yyyy-MM-dd_HHmmss", Locale.US).format(new Date()) + ".zip";
        ContentResolver cr = ctx.getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        v.put(MediaStore.MediaColumns.MIME_TYPE, "application/zip");
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, BACKUP_DIR);
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = cr.insert(MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), v);
        if (uri == null) throw new IOException("could not create the backup in " + BACKUP_DIR);
        try {
            long want = 0;
            try (OutputStream os = cr.openOutputStream(uri, "w")) {
                if (os == null) throw new IOException("could not write the backup");
                ZipOutputStream z = new ZipOutputStream(os);
                byte[] buf = new byte[1 << 16];
                for (File f : files) {
                    z.putNextEntry(new ZipEntry(rel(root, f)));
                    try (InputStream in = new FileInputStream(f)) {
                        for (int r; (r = in.read(buf)) > 0; ) z.write(buf, 0, r);
                    }
                    z.closeEntry();
                    want += f.length();
                }
                z.finish();
            }
            int got = 0;
            long gotBytes = 0;
            try (InputStream is = cr.openInputStream(uri); ZipInputStream z = new ZipInputStream(is)) {
                byte[] buf = new byte[1 << 16];
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    got++;
                    for (int r; (r = z.read(buf)) > 0; ) gotBytes += r; // the CRC is checked as each entry is read
                }
            }
            if (got != files.size() || gotBytes != want) throw new IOException("the backup did not check out");
            ContentValues done = new ContentValues();
            done.put(MediaStore.MediaColumns.IS_PENDING, 0);
            cr.update(uri, done, null, null);
            return name;
        } catch (IOException | RuntimeException e) {
            try { cr.delete(uri, null, null); } catch (Exception ignored) {}
            throw e;
        }
    }

    // ---- helpers ----

    private static boolean isZip(ContentResolver cr, Uri u) throws IOException {
        try (InputStream in = cr.openInputStream(u)) {
            if (in == null) throw new IOException("could not open the file");
            byte[] m = new byte[4];
            int n = 0;
            for (int r; n < 4 && (r = in.read(m, n, 4 - n)) > 0; ) n += r;
            return n == 4 && m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4;
        }
    }

    /** Extracts the regular files, flattened per folder into tmp/<n>/, and groups them by their folder in the zip. */
    private static void extractZip(ContentResolver cr, Uri u, File tmp, Map<String, List<File>> byFolder, long[] total)
            throws IOException {
        Map<String, File> dirs = new HashMap<>();
        int count = 0;
        try (InputStream is = cr.openInputStream(u); ZipInputStream z = new ZipInputStream(is)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory()) continue;
                String p = e.getName().replace('\\', '/');
                if (p.startsWith("__MACOSX/") || p.contains("/__MACOSX/")) continue;
                int slash = p.lastIndexOf('/');
                String folder = slash < 0 ? "" : p.substring(0, slash);
                String n = safeName(p.substring(slash + 1));
                if (n == null) continue;
                if (++count > MAX_FILES) throw new IOException("the zip holds too many files");
                File d = dirs.get(folder);
                if (d == null) {
                    d = new File(tmp, Integer.toString(dirs.size()));
                    if (!d.mkdirs()) throw new IOException("could not create a work folder");
                    dirs.put(folder, d);
                }
                File out = new File(d, n);
                copy(z, out, total);
                List<File> l = byFolder.get(folder);
                if (l == null) byFolder.put(folder, l = new ArrayList<>());
                l.add(out);
            }
        }
    }

    private static void copy(InputStream in, File out, long[] total) throws IOException {
        byte[] buf = new byte[1 << 16];
        try (OutputStream os = new FileOutputStream(out)) {
            for (int r; (r = in.read(buf)) > 0; ) {
                total[0] += r;
                if (total[0] > MAX_TOTAL) throw new IOException("the saves are too large");
                os.write(buf, 0, r);
            }
        }
    }

    /** A plain file name (no folders, not hidden), or null to skip it. */
    private static String safeName(String n) {
        if (n == null) return null;
        n = n.substring(n.lastIndexOf('/') + 1);
        if (n.isEmpty() || n.startsWith(".") || n.length() > 128 || !n.matches("[A-Za-z0-9 _.()-]+")) return null;
        return n;
    }

    private static String displayName(Context ctx, Uri uri) {
        try (Cursor c = ctx.getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (i >= 0) return c.getString(i);
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    private static int depth(String folder) {
        if (folder == null) return Integer.MAX_VALUE;
        if (folder.isEmpty()) return 0;
        int d = 1;
        for (int i = 0; i < folder.length(); i++) if (folder.charAt(i) == '/') d++;
        return d;
    }

    private static void listFiles(File d, List<File> out) {
        File[] l = d.listFiles();
        if (l == null) return;
        java.util.Arrays.sort(l);
        for (File f : l) {
            if (f.isDirectory()) listFiles(f, out);
            else if (f.isFile()) out.add(f);
        }
    }

    private static String rel(File root, File f) {
        return f.getAbsolutePath().substring(root.getAbsolutePath().length() + 1).replace(File.separatorChar, '/');
    }

    static void deleteTree(File f) {
        File[] l = f.listFiles();
        if (l != null) for (File c : l) deleteTree(c);
        f.delete();
    }

    private static void som(String s) { android.util.Log.i("Somniel", s); }
}
