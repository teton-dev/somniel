package dev.teton.somniel.extract;

import java.io.Closeable;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;

/** Random-access read-only bytes: a file, a slice of one, or decrypted data computed on the fly. */
public interface Source extends Closeable {
    long size() throws IOException;

    /** Reads exactly len bytes at pos (throws at the end of the data). */
    void readFully(long pos, byte[] b, int off, int len) throws IOException;

    @Override
    default void close() throws IOException {}

    /** A file (or a content-provider file descriptor) through its channel; positional reads, thread-safe. */
    final class Channel implements Source {
        private final FileChannel ch;
        private final boolean owns;

        public Channel(FileChannel ch, boolean owns) {
            this.ch = ch;
            this.owns = owns;
        }

        @Override public long size() throws IOException { return ch.size(); }

        @Override
        public void readFully(long pos, byte[] b, int off, int len) throws IOException {
            ByteBuffer bb = ByteBuffer.wrap(b, off, len);
            while (bb.hasRemaining()) {
                int n = ch.read(bb, pos + bb.position() - off);
                if (n < 0) throw new IOException("unexpected end of file at " + (pos + bb.position() - off));
            }
        }

        @Override
        public void close() throws IOException {
            if (owns) ch.close();
        }
    }

    /** [offset, offset + length) of another source. */
    final class Slice implements Source {
        private final Source base;
        private final long offset, length;

        public Slice(Source base, long offset, long length) {
            this.base = base;
            this.offset = offset;
            this.length = length;
        }

        @Override public long size() { return length; }

        @Override
        public void readFully(long pos, byte[] b, int off, int len) throws IOException {
            if (pos < 0 || pos + len > length) throw new IOException("read past the end of a slice: " + pos + "+" + len + " of " + length);
            base.readFully(offset + pos, b, off, len);
        }
    }
}
