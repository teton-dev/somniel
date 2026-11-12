package dev.teton.somniel.extract;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;

/**
 * Builds the game data Somniel runs from out of the user's own files: the Engage base game NSP, the 2.0 update NSP and
 * prod.keys (plus title.keys if the ticket cannot supply the title key).
 *
 *   outDir/exefs/{rtld,main,subsdk0,sdk,main.npdm}   from the update's program NCA (the base's if the update has none)
 *   outDir/romfs.bin                                  the base RomFS with the update's BKTR patch applied
 *
 * Both are written under temporary names and renamed at the end, so an interrupted run leaves no half-written data.
 * Pure Java (no Android classes): tools/extract_test checks it on a PC against a known-good extraction.
 */
public final class Extractor {
    public interface Progress {
        void update(String stage, long done, long total);

        boolean cancelled();
    }

    private Extractor() {}

    /** keyFiles: prod.keys and/or title.keys streams. Closes nothing: the caller owns the streams and the sources. */
    public static void run(Source baseNsp, Source updateNsp, InputStream[] keyFiles, File outDir, Progress p) throws IOException {
        Keys keys = new Keys();
        for (InputStream in : keyFiles) keys.load(in);

        p.update("Reading the game files", 0, 0);
        Pfs0 base = new Pfs0(baseNsp), upd = new Pfs0(updateNsp);
        final Map<String, byte[]> tickets = new HashMap<>();
        readTickets(base, tickets);
        readTickets(upd, tickets);
        Nca.TitleKeys tk = rid -> {
            byte[] t = tickets.get(rid);
            return t != null ? t : keys.find(rid);
        };
        Nca baseProg = findProgram(base, keys, tk, "base game");
        Nca updProg = findProgram(upd, keys, tk, "update");

        // exefs: from the update's program NCA (section 0)
        File exefsPart = new File(outDir, "exefs.part");
        deleteTree(exefsPart);
        if (!exefsPart.mkdirs()) throw new IOException("cannot create " + exefsPart);
        Nca.Section ex = updProg.sections[0] != null ? updProg.sections[0] : baseProg.sections[0];
        Nca exNca = updProg.sections[0] != null ? updProg : baseProg;
        if (ex == null || ex.fsType != 1) throw new ExtractException("The program NCA has no exefs section.");
        long[] r = Nca.pfs0Data(ex);
        Pfs0 exefs = new Pfs0(new Source.Slice(exNca.open(ex), r[0], r[1]));
        for (Pfs0.Entry e : exefs.entries) {
            copy(exefs.open(e), new File(exefsPart, e.name), e.size, null, null);
        }
        checkExefs(exefsPart);

        // romfs: the base section patched by the update's BKTR section
        Nca.Section baseRom = romSection(baseProg), updRom = romSection(updProg);
        if (baseRom == null || updRom == null) throw new ExtractException("A program NCA has no RomFS section.");
        Source baseSec = baseProg.open(baseRom);
        Source patched = updRom.encType == 4 ? updProg.openPatch(updRom, baseSec) : updProg.open(updRom);
        long[] data = Nca.ivfcData(updRom);
        File romPart = new File(outDir, "romfs.bin.part");
        if (outDir.getUsableSpace() < data[1] + (512L << 20))
            throw new ExtractException(String.format("Not enough free storage: %.1f GB are needed, %.1f GB are free.",
                    (data[1] + (512L << 20)) / 1e9, outDir.getUsableSpace() / 1e9));
        copy(new Source.Slice(patched, data[0], data[1]), romPart, data[1], p, "Writing the game data");
        byte[] head = new byte[8];
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(romPart, "r")) {
            f.readFully(head);
        }
        if (ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN).getLong() != 0x50)
            throw new ExtractException("The rebuilt RomFS has a bad header: the base game and the update do not match.");

        File exefsDir = new File(outDir, "exefs"), romFile = new File(outDir, "romfs.bin");
        deleteTree(exefsDir);
        romFile.delete();
        if (!exefsPart.renameTo(exefsDir) || !romPart.renameTo(romFile)) throw new IOException("cannot move the finished data into place");
        p.update("Done", data[1], data[1]);
    }

    private static Nca.Section romSection(Nca n) {
        for (Nca.Section s : n.sections) if (s != null && s.fsType == 0) return s;
        return null;
    }

    private static void checkExefs(File dir) throws IOException {
        File main = new File(dir, "main");
        if (!main.isFile() || !new File(dir, "main.npdm").isFile()) throw new ExtractException("The exefs is missing main or main.npdm.");
        byte[] m = new byte[4];
        try (java.io.FileInputStream in = new java.io.FileInputStream(main)) {
            if (in.read(m) != 4 || m[0] != 'N' || m[1] != 'S' || m[2] != 'O' || m[3] != '0') throw new ExtractException("exefs/main is not an NSO.");
        }
    }

    private static Nca findProgram(Pfs0 nsp, Keys keys, Nca.TitleKeys tk, String what) throws IOException {
        IOException first = null;
        for (Pfs0.Entry e : nsp.entries) {
            if (!e.name.endsWith(".nca") || e.name.endsWith(".cnmt.nca")) continue;
            try {
                Nca n = new Nca(nsp.open(e), keys, tk);
                if (n.contentType == Nca.TYPE_PROGRAM) return n;
            } catch (ExtractException ex) {
                if (first == null) first = ex;
            }
        }
        if (first != null) throw first;
        throw new ExtractException("The " + what + " NSP has no program content.");
    }

    /** Common tickets: the (encrypted) title key by rights id. Personalized tickets cannot be used without the console's key. */
    private static void readTickets(Pfs0 nsp, Map<String, byte[]> out) throws IOException {
        for (Pfs0.Entry e : nsp.entries) {
            if (!e.name.endsWith(".tik") || e.size < 0x2C0 || e.size > 0x4000) continue;
            byte[] t = new byte[(int) e.size];
            nsp.open(e).readFully(0, t, 0, t.length);
            ByteBuffer b = ByteBuffer.wrap(t).order(ByteOrder.LITTLE_ENDIAN);
            int sig = b.getInt(0);
            int sigSize = (sig == 0x10000 || sig == 0x10003) ? 0x200 : (sig == 0x10001 || sig == 0x10004) ? 0x100
                    : (sig == 0x10002 || sig == 0x10005) ? 0x3C : -1;
            if (sigSize < 0) continue;
            int body = (4 + sigSize + 0x3F) & ~0x3F;
            if (body + 0x170 > t.length) continue;
            if ((t[body + 0x141] & 0xff) != 0) continue; // personalized (RSA-wrapped) key
            byte[] key = new byte[16];
            System.arraycopy(t, body + 0x40, key, 0, 16);
            out.put(Keys.hex(t, body + 0x160, 16), key);
        }
    }

    private static void copy(Source from, File to, long size, Progress p, String stage) throws IOException {
        byte[] buf = new byte[8 << 20];
        long done = 0;
        long t0 = System.nanoTime();
        try (OutputStream out = new FileOutputStream(to)) {
            while (done < size) {
                if (p != null && p.cancelled()) throw new IOException("Cancelled");
                int n = (int) Math.min(buf.length, size - done);
                from.readFully(done, buf, 0, n);
                out.write(buf, 0, n);
                done += n;
                if (p != null) p.update(stage, done, size);
            }
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
