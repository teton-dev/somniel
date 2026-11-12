package dev.teton.somniel.extract;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** A PFS0 container (an NSP, or an exefs): names and the byte ranges of the files in it. */
final class Pfs0 {
    static final class Entry {
        final String name;
        final long offset, size; // relative to the start of the PFS0 source

        Entry(String name, long offset, long size) {
            this.name = name;
            this.offset = offset;
            this.size = size;
        }
    }

    final List<Entry> entries = new ArrayList<>();
    final Source src;

    Pfs0(Source src) throws IOException {
        this.src = src;
        byte[] h = new byte[16];
        src.readFully(0, h, 0, 16);
        ByteBuffer b = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
        if (b.getInt(0) != 0x30534650) throw new ExtractException("Not an NSP/PFS0 file (bad header).");
        int count = b.getInt(4), strSize = b.getInt(8);
        if (count < 1 || count > 100000 || strSize < 0 || strSize > (16 << 20)) throw new ExtractException("Corrupt PFS0 header.");
        byte[] tbl = new byte[count * 24 + strSize];
        src.readFully(16, tbl, 0, tbl.length);
        ByteBuffer t = ByteBuffer.wrap(tbl).order(ByteOrder.LITTLE_ENDIAN);
        long dataStart = 16L + tbl.length;
        for (int i = 0; i < count; i++) {
            long off = t.getLong(i * 24), size = t.getLong(i * 24 + 8);
            int nameOff = t.getInt(i * 24 + 16);
            int end = count * 24 + nameOff;
            int e = end;
            while (e < tbl.length && tbl[e] != 0) e++;
            String name = new String(tbl, end, e - end, StandardCharsets.ISO_8859_1);
            entries.add(new Entry(name, dataStart + off, size));
        }
    }

    Source open(Entry e) { return new Source.Slice(src, e.offset, e.size); }
}
