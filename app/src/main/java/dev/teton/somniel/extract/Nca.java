package dev.teton.somniel.extract;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

/**
 * One NCA: header (AES-XTS with header_key), key area, and its sections as decrypted Sources.
 * Section crypto supported: none, AES-CTR (3) and AES-CTR-EX (4, the update's BKTR patch). NCA2 and XTS sections
 * do not occur in retail Switch titles of this kind.
 */
final class Nca {
    static final int TYPE_PROGRAM = 0;

    final Source src;
    final byte[] hdr = new byte[0xC00];
    final int contentType, keyGen;
    final byte[] rightsId = new byte[16];
    final boolean hasRights;
    private final byte[] ctrKey;

    static final class Section {
        int index;
        long start, end; // bytes within the NCA
        int fsType, hashType, encType;
        byte[] fsHeader = new byte[0x200];
        byte[] ctr8 = new byte[8];
    }

    final Section[] sections = new Section[4];

    /** titleKeyFor is asked for the (encrypted) title key of the rights id; may return null. */
    interface TitleKeys { byte[] encryptedKey(String rightsIdHex); }

    Nca(Source src, Keys keys, TitleKeys titleKeys) throws IOException {
        this.src = src;
        byte[] raw = new byte[0xC00];
        src.readFully(0, raw, 0, 0xC00);
        byte[] headerKey = keys.need("header_key");
        for (int s = 0; s < 6; s++) { // 0x200-byte XTS sectors, numbered from the start of the file
            System.arraycopy(raw, s * 0x200, hdr, s * 0x200, 0x200);
            Crypto.xtsDecrypt(headerKey, s, hdr, s * 0x200, 0x200);
        }
        ByteBuffer b = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN);
        int magic = b.getInt(0x200);
        if (magic != 0x3341434E && magic != 0x3241434E) // "NCA3" / "NCA2"
            throw new ExtractException("An NCA did not decrypt: wrong header_key in your keys file?");
        if (magic == 0x3241434E) throw new ExtractException("NCA2 files are not supported.");
        contentType = hdr[0x205] & 0xff;
        int ct1 = hdr[0x206] & 0xff, ct2 = hdr[0x220] & 0xff;
        int gen = Math.max(ct1, ct2);
        keyGen = gen > 0 ? gen - 1 : 0;
        System.arraycopy(hdr, 0x230, rightsId, 0, 16);
        hasRights = !allZero(rightsId, 0, 16);
        int keyIndex = hdr[0x207] & 0xff;

        if (hasRights) {
            String rid = Keys.hex(rightsId, 0, 16);
            byte[] enc = titleKeys.encryptedKey(rid);
            if (enc == null) throw new ExtractException("No title key for rights id " + rid
                    + ": the NSP has no usable ticket and your title.keys has no entry for it.");
            ctrKey = Crypto.ecbDecrypt(keys.need(String.format("titlekek_%02x", keyGen)), Arrays.copyOf(enc, 16));
        } else {
            String name = keyIndex == 0 ? "key_area_key_application_" : keyIndex == 1 ? "key_area_key_ocean_" : "key_area_key_system_";
            byte[] kak = keys.need(String.format("%s%02x", name, keyGen));
            byte[] area = Crypto.ecbDecrypt(kak, Arrays.copyOfRange(hdr, 0x300, 0x340));
            ctrKey = Arrays.copyOfRange(area, 0x20, 0x30); // key 2 is the AES-CTR key
        }

        for (int i = 0; i < 4; i++) {
            long st = (b.getInt(0x240 + i * 0x10) & 0xffffffffL) * 0x200, en = (b.getInt(0x244 + i * 0x10) & 0xffffffffL) * 0x200;
            if (en <= st) continue;
            Section s = new Section();
            s.index = i;
            s.start = st;
            s.end = en;
            System.arraycopy(hdr, 0x400 + i * 0x200, s.fsHeader, 0, 0x200);
            s.fsType = s.fsHeader[2] & 0xff;
            s.hashType = s.fsHeader[3] & 0xff;
            s.encType = s.fsHeader[4] & 0xff;
            for (int k = 0; k < 8; k++) s.ctr8[k] = s.fsHeader[0x147 - k]; // 8 bytes at 0x140, reversed
            sections[i] = s;
        }
    }

    private static boolean allZero(byte[] a, int off, int len) {
        for (int i = 0; i < len; i++) if (a[off + i] != 0) return false;
        return true;
    }

    /** The section's data, decrypted (not for a BKTR patch: see Bktr). */
    Source open(Section s) throws IOException {
        if (s.encType == 4) throw new ExtractException("A patch section needs its base game.");
        if (s.encType == 1) return new Source.Slice(src, s.start, s.end - s.start);
        if (s.encType != 3) throw new ExtractException("Unsupported NCA section encryption " + s.encType);
        final Section sec = s;
        return new Source() {
            @Override public long size() { return sec.end - sec.start; }

            @Override
            public void readFully(long pos, byte[] b, int off, int len) throws IOException {
                if (pos < 0 || pos + len > sec.end - sec.start) throw new IOException("read past the end of an NCA section");
                Crypto.ctrRead(src, sec.start, pos, ctrKey, sec.ctr8, -1, b, off, len);
            }
        };
    }

    /** The RomFS image's offset and size inside a RomFS section's data (the IVFC data level). */
    static long[] ivfcData(Section s) throws ExtractException {
        ByteBuffer b = ByteBuffer.wrap(s.fsHeader).order(ByteOrder.LITTLE_ENDIAN);
        if (b.getInt(8) != 0x43465649) throw new ExtractException("The RomFS section has no IVFC header.");
        int levels = b.getInt(0x14) - 1; // 7 levels counting the master hash: 6 described
        int at = 0x18 + (levels - 1) * 0x18;
        return new long[] {b.getLong(at), b.getLong(at + 8)};
    }

    /** The exefs PFS0 data region inside a PFS0 section's data. */
    static long[] pfs0Data(Section s) {
        ByteBuffer b = ByteBuffer.wrap(s.fsHeader).order(ByteOrder.LITTLE_ENDIAN);
        return new long[] {b.getLong(0x40), b.getLong(0x48)}; // hierarchical SHA-256 layer regions start at 0x30; region 1 holds the PFS0
    }

    /**
     * A patch section (AES-CTR-EX) merged with the base game's section: the virtual section the game sees. Entries
     * come from two bucket trees stored (AES-CTR) at the end of the section: relocations pick base or patch data for
     * each range, subsections give the counter generation for the patch's own data.
     */
    Source openPatch(Section s, Source baseSection) throws IOException {
        ByteBuffer b = ByteBuffer.wrap(s.fsHeader).order(ByteOrder.LITTLE_ENDIAN);
        long relOff = b.getLong(0x100), relSize = b.getLong(0x108);
        long subOff = b.getLong(0x120), subSize = b.getLong(0x128);
        if (b.getInt(0x110) != 0x52544B42 || b.getInt(0x130) != 0x52544B42) // "BKTR"
            throw new ExtractException("The patch section has no BKTR tables.");
        final long dataEnd = relOff; // the patch's own data ends where the tables begin
        byte[] rel = new byte[(int) relSize], sub = new byte[(int) subSize];
        Crypto.ctrRead(src, s.start, relOff, ctrKey, s.ctr8, -1, rel, 0, rel.length);
        Crypto.ctrRead(src, s.start, subOff, ctrKey, s.ctr8, -1, sub, 0, sub.length);
        final long[][] relocs = parseTree(rel, 0x14, b.getInt(0x118));
        final long[][] subs = parseTree(sub, 0x10, b.getInt(0x138));
        final long virtualSize = ByteBuffer.wrap(rel).order(ByteOrder.LITTLE_ENDIAN).getLong(8);
        final long[] rVirt = relocs[0], rPhys = relocs[1], rStore = relocs[2];
        final long[] sOff = subs[0], sGen = subs[2];
        final Section sec = s;
        return new Source() {
            @Override public long size() { return virtualSize; }

            @Override
            public void readFully(long pos, byte[] out, int off, int len) throws IOException {
                if (pos < 0 || pos + len > virtualSize) throw new IOException("read past the end of the patched section");
                while (len > 0) {
                    int i = floor(rVirt, pos);
                    long end = i + 1 < rVirt.length ? rVirt[i + 1] : virtualSize;
                    long phys = rPhys[i] + (pos - rVirt[i]);
                    int n = (int) Math.min(len, end - pos);
                    if (rStore[i] == 0) {
                        baseSection.readFully(phys, out, off, n);
                    } else {
                        int k = floor(sOff, phys); // the counter generation changes at subsection boundaries
                        long subEnd = k + 1 < sOff.length ? sOff[k + 1] : dataEnd;
                        n = (int) Math.min(n, subEnd - phys);
                        if (n <= 0) throw new IOException("patch data outside its subsections at " + phys);
                        Crypto.ctrRead(src, sec.start, phys, ctrKey, sec.ctr8, (int) sGen[k], out, off, n);
                    }
                    pos += n;
                    off += n;
                    len -= n;
                }
            }
        };
    }

    /** Index of the last entry whose key is <= v. */
    private static int floor(long[] keys, long v) {
        int lo = 0, hi = keys.length - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) >>> 1;
            if (keys[mid] <= v) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }

    /**
     * A bucket tree: node 0 (0x4000 bytes) lists the buckets, each bucket is one 0x4000 node of entries after it. Returns
     * columns {first u64, second u64, third (u32 or the u64 field)} per entry: relocation entries are (virtual, physical,
     * storage index), subsection entries (offset, unused, generation).
     */
    private static long[][] parseTree(byte[] t, int entrySize, int expect) throws IOException {
        ByteBuffer b = ByteBuffer.wrap(t).order(ByteOrder.LITTLE_ENDIAN);
        int buckets = b.getInt(4);
        long[] c0 = new long[expect], c1 = new long[expect], c2 = new long[expect];
        int n = 0;
        for (int k = 0; k < buckets; k++) {
            int base = (k + 1) * 0x4000;
            int count = b.getInt(base + 4);
            for (int e = 0; e < count; e++) {
                int p = base + 0x10 + e * entrySize;
                if (n >= expect) throw new ExtractException("BKTR table has more entries than its header says.");
                c0[n] = b.getLong(p);
                if (entrySize == 0x14) {
                    c1[n] = b.getLong(p + 8);
                    c2[n] = b.getInt(p + 16);
                } else {
                    c2[n] = b.getInt(p + 12); // subsection: offset u64, padding u32, generation u32
                }
                n++;
            }
        }
        if (n != expect) throw new ExtractException("BKTR table has " + n + " entries, expected " + expect + ".");
        return new long[][] {c0, c1, c2};
    }
}
