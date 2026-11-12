package dev.teton.somniel.extract;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** prod.keys and title.keys ("name = hex" lines). Names are matched lower-case. */
final class Keys {
    private final Map<String, byte[]> map = new HashMap<>();

    /** Adds every "name = hex" line of the stream (prod.keys or title.keys; both have the same shape). */
    void load(InputStream in) throws IOException {
        BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
        String line;
        while ((line = r.readLine()) != null) {
            int eq = line.indexOf('=');
            if (eq < 0 || line.trim().startsWith(";") || line.trim().startsWith("#")) continue;
            String name = line.substring(0, eq).trim().toLowerCase(Locale.ROOT);
            String hex = line.substring(eq + 1).trim();
            if (name.isEmpty() || hex.isEmpty() || hex.length() % 2 != 0 || !hex.matches("[0-9a-fA-F]+")) continue;
            map.put(name, fromHex(hex));
        }
    }

    byte[] find(String name) { return map.get(name.toLowerCase(Locale.ROOT)); }

    byte[] need(String name) throws ExtractException {
        byte[] k = find(name);
        if (k == null) throw new ExtractException("Your keys file has no " + name + ". It must be a full prod.keys "
                + "(from Lockpick_RCM) for the game's key generation.");
        return k;
    }

    static String hex(byte[] b, int off, int len) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < len; i++) sb.append(String.format("%02x", b[off + i] & 0xff));
        return sb.toString();
    }

    static byte[] fromHex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return out;
    }
}
