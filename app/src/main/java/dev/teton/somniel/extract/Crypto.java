package dev.teton.somniel.extract;

import java.io.IOException;
import java.security.GeneralSecurityException;

import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/** AES helpers for NCA files: ECB (key unwrapping), XTS (the NCA header) and CTR (section data). */
final class Crypto {
    private Crypto() {}

    static byte[] ecbDecrypt(byte[] key, byte[] data) throws IOException {
        try {
            Cipher c = Cipher.getInstance("AES/ECB/NoPadding");
            c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"));
            return c.doFinal(data);
        } catch (GeneralSecurityException e) {
            throw new IOException("AES failed: " + e.getMessage(), e);
        }
    }

    /** AES-XTS decrypt of one sector in place. Nintendo's tweak is the sector number big-endian; key = data key + tweak key. */
    static void xtsDecrypt(byte[] key32, long sector, byte[] buf, int off, int len) throws IOException {
        try {
            byte[] k1 = new byte[16], k2 = new byte[16];
            System.arraycopy(key32, 0, k1, 0, 16);
            System.arraycopy(key32, 16, k2, 0, 16);
            Cipher dec = Cipher.getInstance("AES/ECB/NoPadding");
            dec.init(Cipher.DECRYPT_MODE, new SecretKeySpec(k1, "AES"));
            Cipher enc = Cipher.getInstance("AES/ECB/NoPadding");
            enc.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(k2, "AES"));
            byte[] t = new byte[16];
            for (int i = 0; i < 8; i++) t[15 - i] = (byte) (sector >>> (8 * i));
            t = enc.doFinal(t);
            byte[] blk = new byte[16];
            for (int p = off; p < off + len; p += 16) {
                for (int i = 0; i < 16; i++) blk[i] = (byte) (buf[p + i] ^ t[i]);
                byte[] y = dec.doFinal(blk);
                for (int i = 0; i < 16; i++) buf[p + i] = (byte) (y[i] ^ t[i]);
                int carry = 0; // t *= x in GF(2^128), little-endian as in IEEE XTS
                for (int i = 0; i < 16; i++) {
                    int nc = (t[i] >> 7) & 1;
                    t[i] = (byte) ((t[i] << 1) | carry);
                    carry = nc;
                }
                if (carry != 0) t[0] ^= (byte) 0x87;
            }
        } catch (GeneralSecurityException e) {
            throw new IOException("AES-XTS failed: " + e.getMessage(), e);
        }
    }

    /**
     * Reads len bytes at pos of an AES-CTR encrypted region and decrypts them. The region starts at absolute offset
     * start of raw; the counter is ctr8 (the section's upper counter, 8 bytes, big-endian) followed by absolute offset / 16.
     * generation >= 0 replaces counter bytes 4..8 (a BKTR patch's subsection generation).
     */
    static void ctrRead(Source raw, long start, long pos, byte[] key, byte[] ctr8, int generation, byte[] b, int off, int len)
            throws IOException {
        long abs = start + pos;
        int lead = (int) (abs & 15);
        long alignedAbs = abs - lead;
        byte[] iv = new byte[16];
        System.arraycopy(ctr8, 0, iv, 0, 8);
        if (generation >= 0) {
            iv[4] = (byte) (generation >>> 24);
            iv[5] = (byte) (generation >>> 16);
            iv[6] = (byte) (generation >>> 8);
            iv[7] = (byte) generation;
        }
        long blocks = alignedAbs >>> 4;
        for (int i = 0; i < 8; i++) iv[15 - i] = (byte) (blocks >>> (8 * i));
        try {
            Cipher c = Cipher.getInstance("AES/CTR/NoPadding");
            c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new IvParameterSpec(iv));
            if (lead == 0 && (len & 15) == 0) { // the common case: whole blocks, decrypted where they were read
                raw.readFully(abs, b, off, len);
                c.update(b, off, len, b, off);
                return;
            }
            int total = lead + len;
            byte[] in = new byte[(total + 15) & ~15]; // the tail past the data stays zero and is discarded
            raw.readFully(alignedAbs, in, 0, total);
            byte[] out = c.doFinal(in);
            System.arraycopy(out, lead, b, off, len);
        } catch (GeneralSecurityException e) {
            throw new IOException("AES-CTR failed: " + e.getMessage(), e);
        }
    }
}
