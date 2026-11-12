package dev.teton.somniel;

import android.content.Context;
import android.net.Uri;

import org.apache.commons.compress.archivers.sevenz.SevenZArchiveEntry;
import org.apache.commons.compress.archivers.sevenz.SevenZFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Installs a mod archive (.zip or .7z) as a Cobalt mod: the folder that holds Data/ or patches/ (the archive may wrap
 * it in other folders, or hold several mods side by side) is moved to <external files>/sd/engage/mods/&lt;name&gt;/.
 * Extraction goes to a scratch folder first, so a broken archive never leaves half a mod where Cobalt scans.
 */
final class ModImporter {
    interface Progress { void update(String message); }

    /** What an import did, for the toast: the installed mod names and whether any of them is switched on. */
    static final class Result {
        final List<String> names = new ArrayList<>();
        boolean anyEnabled;
        String message;
    }

    private static final String[] MOD_DIRS = {"data", "patches", "plugins"};

    static File modsRoot(Context ctx) {
        File ext = ctx.getExternalFilesDir(null);
        return ext == null ? null : new File(ext, "sd/engage");
    }

    static Result importArchive(Context ctx, Uri uri, String shownName, Progress progress) throws IOException {
        File engage = modsRoot(ctx);
        if (engage == null) throw new IOException("storage is not available");
        File tmp = new File(engage, ".import_tmp");
        deleteTree(tmp);
        if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
        try {
            byte[] magic = new byte[6];
            int got;
            try (InputStream in = ctx.getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("cannot open the file");
                got = in.read(magic);
            }
            if (got >= 4 && magic[0] == 'P' && magic[1] == 'K') {
                progress.update("Extracting " + shownName + "...");
                extractZip(ctx, uri, tmp, progress);
            } else if (got >= 6 && (magic[0] & 0xFF) == 0x37 && magic[1] == 0x7A && (magic[2] & 0xFF) == 0xBC
                    && (magic[3] & 0xFF) == 0xAF && magic[4] == 0x27 && magic[5] == 0x1C) {
                progress.update("Reading " + shownName + "...");
                extract7z(ctx, uri, tmp, progress);
            } else {
                throw new IOException("only .zip and .7z archives are supported");
            }
            progress.update("Installing...");
            return install(tmp, shownName, engage);
        } finally {
            deleteTree(tmp);
        }
    }

    // ---- extraction ----

    private static File safeTarget(File root, String entryName) throws IOException {
        String n = entryName.replace('\\', '/');
        while (n.startsWith("/")) n = n.substring(1);
        if (n.isEmpty()) return null;
        File out = new File(root, n);
        String rootPath = root.getCanonicalPath() + File.separator;
        if (!out.getCanonicalPath().startsWith(rootPath)) return null; // ../ or an absolute path: skipped
        return out;
    }

    private static void extractZip(Context ctx, Uri uri, File dest, Progress progress) throws IOException {
        byte[] buf = new byte[1 << 16];
        int files = 0;
        try (InputStream in = ctx.getContentResolver().openInputStream(uri);
             ZipInputStream zin = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                File out = safeTarget(dest, e.getName());
                if (out == null) continue;
                if (e.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                writeTo(out, zin, buf);
                if (++files % 25 == 0) progress.update("Extracting... " + files + " files");
            }
        }
    }

    private static void extract7z(Context ctx, Uri uri, File dest, Progress progress) throws IOException {
        // the 7z reader needs random access: spool the archive to the app's cache first
        File spool = new File(ctx.getCacheDir(), "import.7z");
        try {
            byte[] buf = new byte[1 << 16];
            try (InputStream in = ctx.getContentResolver().openInputStream(uri)) {
                writeTo(spool, in, buf);
            }
            int files = 0;
            try (SevenZFile sz = SevenZFile.builder().setFile(spool).get()) {
                SevenZArchiveEntry e;
                while ((e = sz.getNextEntry()) != null) {
                    File out = safeTarget(dest, e.getName());
                    if (out == null) continue;
                    if (e.isDirectory()) {
                        out.mkdirs();
                        continue;
                    }
                    File parent = out.getParentFile();
                    if (parent != null) parent.mkdirs();
                    try (OutputStream os = new FileOutputStream(out)) {
                        int n;
                        while ((n = sz.read(buf)) > 0) os.write(buf, 0, n);
                    }
                    if (++files % 25 == 0) progress.update("Extracting... " + files + " files");
                }
            }
        } finally {
            spool.delete();
        }
    }

    private static void writeTo(File out, InputStream in, byte[] buf) throws IOException {
        File parent = out.getParentFile();
        if (parent != null) parent.mkdirs();
        try (OutputStream os = new FileOutputStream(out)) {
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        }
    }

    // ---- finding and installing the mod folders ----

    private static boolean looksLikeMod(File dir) {
        File[] kids = dir.listFiles();
        if (kids == null) return false;
        for (File k : kids) {
            String n = k.getName().toLowerCase(java.util.Locale.ROOT);
            if (k.isDirectory()) {
                for (String m : MOD_DIRS) if (n.equals(m)) return true;
            } else if (n.endsWith(".nro") || n.endsWith(".lua")) {
                return true;
            }
        }
        return false;
    }

    private static File[] visibleKids(File dir) {
        File[] kids = dir.listFiles((d, n) -> !n.startsWith(".") && !n.equals("__MACOSX"));
        return kids == null ? new File[0] : kids;
    }

    private static Result install(File tmp, String archiveName, File engage) throws IOException {
        // walk down single wrapper folders until the mod's own content shows
        File root = tmp;
        String wrapper = null;
        while (!looksLikeMod(root)) {
            File[] kids = visibleKids(root);
            if (kids.length == 1 && kids[0].isDirectory()) {
                root = kids[0];
                wrapper = root.getName();
            } else {
                break;
            }
        }
        List<File> mods = new ArrayList<>();
        List<String> names = new ArrayList<>();
        if (looksLikeMod(root)) {
            mods.add(root);
            names.add(wrapper != null ? wrapper : baseName(archiveName));
        } else { // several mods side by side?
            for (File k : visibleKids(root))
                if (k.isDirectory() && looksLikeMod(k)) {
                    mods.add(k);
                    names.add(k.getName());
                }
        }
        if (mods.isEmpty())
            throw new IOException("no mod found in the archive (expected a folder with Data, patches or plugins)");

        File on = new File(engage, "mods"), off = new File(engage, "mods_disabled");
        Result r = new Result();
        for (int i = 0; i < mods.size(); i++) {
            String name = safeName(names.get(i));
            File target = new File(off, name).isDirectory() ? new File(off, name) : new File(on, name);
            deleteTree(target); // an updated mod replaces the old copy (and keeps its on/off state)
            File parent = target.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            if (!mods.get(i).renameTo(target)) throw new IOException("cannot move the mod into " + target);
            r.names.add(name);
            r.anyEnabled |= target.getParentFile().equals(on);
        }
        r.message = "Installed " + String.join(", ", r.names) + ".";
        return r;
    }

    private static String baseName(String s) {
        return s.replaceAll("(?i)\\.(zip|7z)$", "");
    }

    static String safeName(String s) {
        s = s.replaceAll("[^A-Za-z0-9 _.()\\-]+", "_").trim().replaceAll("^[._ ]+", "");
        if (s.length() > 80) s = s.substring(0, 80);
        return s.isEmpty() ? "Mod" : s;
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
