package dev.teton.somniel;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;

/**
 * First-run setup: shown when the game's files are not on the device. It asks for the Engage base game .nsp, the
 * Engage 2.0 update .nsp and the key files (prod.keys, and title.keys if the user has one).
 *
 * The NSPs are not copied (they are many GB): the picker's permission is kept and the files are read in place when the
 * game data is built. The key files are small and are stored in the app's private storage. Each choice is checked
 * before it is accepted: the NSP must be a PFS0 container whose ticket names the right title, the keys file must
 * hold prod keys or title keys.
 *
 * NOT built yet: turning the chosen files into the game data (decrypting the NCAs and building exefs and romfs on the
 * device). Until then, "Continue" saves the choices and says so.
 */
final class SetupDialog {
    static final int REQ_BASE = 0x5D10, REQ_UPDATE = 0x5D11, REQ_KEYS = 0x5D12;
    private static final String BASE_ID = "0100A6301214E000", UPDATE_ID = "0100A6301214E800";
    private static final Pattern TITLE_KEY = Pattern.compile("(?m)^\\s*[0-9a-fA-F]{32}\\s*=\\s*[0-9a-fA-F]{32}\\s*$");

    private final SomnielActivity act;
    private final SharedPreferences prefs;
    private final float dp;
    private final FrameLayout root;
    private TextView baseStatus, updateStatus, keysStatus, locStatus, spaceNote;
    private Button cont, later;
    private boolean showing;

    SetupDialog(SomnielActivity act) {
        this.act = act;
        this.prefs = act.getSharedPreferences("setup", Context.MODE_PRIVATE);
        this.dp = act.getResources().getDisplayMetrics().density;

        root = new FrameLayout(act);
        root.setBackgroundColor(0xF0101420);
        root.setVisibility(View.GONE);
        root.setClickable(true); // nothing reaches the game surface underneath

        LinearLayout card = new LinearLayout(act);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(0xFF1A2030);
        card.setPadding(px(24), px(18), px(24), px(18));
        // The card fills the screen's height (so it never runs off it, in landscape too): the title stays on top, the
        // rows scroll in the middle, and the progress and the buttons stay pinned at the bottom, always reachable.
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(px(520), -1, Gravity.CENTER);
        lp.setMargins(px(16), px(8), px(16), px(8));
        root.addView(card, lp);

        card.addView(text("Set up Somniel", 24, true, Color.WHITE));
        card.addView(text("Somniel needs your own copy of Fire Emblem Engage. Choose the files below; the game's "
                + "shaders are then built on this device.", 14, false, 0xFFB0B8CC), pad(0, 6, 8, 12));

        LinearLayout body = new LinearLayout(act);
        body.setOrientation(LinearLayout.VERTICAL);
        ScrollView sv = new ScrollView(act);
        sv.addView(body);
        card.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));

        baseStatus = status();
        body.addView(pickRow("Engage base game (.nsp)", baseStatus, v -> act.pickFile(REQ_BASE, nspMimes(), false)));
        updateStatus = status();
        body.addView(pickRow("Engage 2.0 update (.nsp)", updateStatus, v -> act.pickFile(REQ_UPDATE, nspMimes(), false)));
        keysStatus = status();
        body.addView(pickRow("Keys (prod.keys, and title.keys if you have it)", keysStatus,
                v -> act.pickFile(REQ_KEYS, new String[] {"*/*"}, true)));

        locStatus = status();
        body.addView(pickRow("Install to", locStatus, v -> chooseLocation()));
        spaceNote = text("", 12, false, 0xFFB0B8CC);
        body.addView(spaceNote, pad(0, 10, 0, 6));

        LinearLayout btns = new LinearLayout(act);
        btns.setOrientation(LinearLayout.HORIZONTAL);
        btns.setGravity(Gravity.END);
        bar = new android.widget.ProgressBar(act, null, android.R.attr.progressBarStyleHorizontal);
        bar.setVisibility(View.GONE);
        card.addView(bar, pad(0, 6, 0, 0));
        stageText = text("", 13, false, 0xFFD2D8E8);
        stageText.setVisibility(View.GONE);
        card.addView(stageText);
        later = button("Not now", v -> hide());
        cont = button("Continue", v -> onContinue());
        btns.addView(later);
        btns.addView(cont);
        card.addView(btns, pad(0, 8, 0, 0));
        refresh();
    }

    View view() { return root; }
    boolean isShowing() { return showing; }

    /** True when exefs and the packed romfs are in the app's data folder (nothing to ask for). */
    static boolean gameFilesPresent(Context c) {
        File ext = c.getExternalFilesDir(null);
        if (ext == null) return false;
        File dir = ext;
        File gd = new File(ext, "game_dir"); // set when the game was installed on another volume (the native side reads it too)
        if (gd.isFile()) {
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(gd))) {
                String line = r.readLine();
                if (line != null && !line.trim().isEmpty() && new File(line.trim(), "exefs/main").isFile()) dir = new File(line.trim());
            } catch (IOException ignored) {
            }
        }
        return new File(dir, "exefs/main").isFile() && new File(dir, "romfs.bin").isFile();
    }

    // ---- where to install ----

    private static final long NEEDED = 20L << 30; // the RomFS is about 16.5 GB, plus working room

    private static final class Loc {
        File dir;
        String label;
        boolean removable;
    }

    /** The app's folder on each mounted volume: internal storage first, then an SD card or other removable volume. */
    private java.util.List<Loc> locations() {
        java.util.List<Loc> out = new java.util.ArrayList<>();
        android.os.storage.StorageManager sm = (android.os.storage.StorageManager) act.getSystemService(Context.STORAGE_SERVICE);
        for (File f : act.getExternalFilesDirs(null)) {
            if (f == null || !android.os.Environment.MEDIA_MOUNTED.equals(android.os.Environment.getExternalStorageState(f))) continue;
            Loc l = new Loc();
            l.dir = f;
            android.os.storage.StorageVolume v = sm.getStorageVolume(f);
            l.removable = v != null ? v.isRemovable() : !out.isEmpty();
            String d = v != null ? v.getDescription(act) : null;
            l.label = !l.removable ? "Internal storage" : d != null ? d : "SD card";
            out.add(l);
        }
        return out;
    }

    /** The saved choice if that volume is still there; otherwise internal storage, or the card when internal is too small. */
    private Loc selectedLoc() {
        java.util.List<Loc> all = locations();
        if (all.isEmpty()) return null;
        String saved = prefs.getString("install_dir", null);
        for (Loc l : all) if (l.dir.getAbsolutePath().equals(saved)) return l;
        Loc first = all.get(0);
        if (first.dir.getUsableSpace() < NEEDED)
            for (Loc l : all) if (l.dir.getUsableSpace() >= NEEDED) return l;
        return first;
    }

    private static String locText(Loc l) {
        long free = l.dir.getUsableSpace();
        return String.format(java.util.Locale.US, "%s: %.1f GB free%s", l.label, free / 1e9, free < NEEDED ? " (not enough)" : "");
    }

    private void chooseLocation() {
        final java.util.List<Loc> all = locations();
        if (all.size() < 2) {
            Toast.makeText(act, "No SD card found: the game data goes on internal storage.", Toast.LENGTH_LONG).show();
            return;
        }
        String[] names = new String[all.size()];
        int cur = 0;
        Loc sel = selectedLoc();
        for (int i = 0; i < names.length; i++) {
            names[i] = locText(all.get(i));
            if (all.get(i) == sel || (sel != null && all.get(i).dir.equals(sel.dir))) cur = i;
        }
        new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Install the game data to")
                .setSingleChoiceItems(names, cur, (d, which) -> {
                    prefs.edit().putString("install_dir", all.get(which).dir.getAbsolutePath()).apply();
                    d.dismiss();
                    refresh();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    void show() {
        showing = true;
        refresh();
        root.setVisibility(View.VISIBLE);
        SomnielActivity.nativeSidebarOpen(true); // the game's input is held while this is up
        (cont.isEnabled() ? cont : root.findFocus() != null ? root.findFocus() : cont).requestFocus();
    }

    void hide() {
        showing = false;
        root.setVisibility(View.GONE);
        SomnielActivity.nativeSidebarOpen(false);
        act.sidebarClosed();
    }

    // ---- choices ----

    private String saved(String k) { return prefs.getString(k, null); }
    private File keysDir() { return new File(act.getFilesDir(), "keys"); }
    private boolean haveProdKeys() { return new File(keysDir(), "prod.keys").isFile(); }

    private void refresh() {
        baseStatus.setText(saved("base") != null ? "✓ " + saved("base_name") : "Not chosen");
        updateStatus.setText(saved("update") != null ? "✓ " + saved("update_name") : "Not chosen");
        boolean tk = new File(keysDir(), "title.keys").isFile();
        keysStatus.setText(haveProdKeys() ? "✓ prod.keys" + (tk ? " and title.keys" : "") : tk ? "title.keys only: prod.keys is needed too" : "Not chosen");
        cont.setEnabled(saved("base") != null && saved("update") != null && haveProdKeys());
        cont.setAlpha(cont.isEnabled() ? 1f : 0.4f);
        Loc loc = selectedLoc();
        locStatus.setText(loc == null ? "No storage available" : locText(loc) + (locations().size() > 1 ? "" : "  (no SD card found)"));
        spaceNote.setText("About 20 GB of free storage is needed for the game data. The .nsp files are read in place, "
                + "not copied. An SD card must be fast (UHS-I or better): the game streams from it while it runs, and "
                + "the data is removed with the app.");
    }

    void onPicked(int req, int res, Intent data) {
        if (res != android.app.Activity.RESULT_OK || data == null) return;
        try {
            if (req == REQ_KEYS) {
                java.util.List<Uri> uris = new java.util.ArrayList<>();
                if (data.getClipData() != null)
                    for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
                else if (data.getData() != null) uris.add(data.getData());
                String msg = null;
                for (Uri u : uris) {
                    String r = acceptKeys(u);
                    if (r != null) msg = r;
                }
                if (msg != null) Toast.makeText(act, msg, Toast.LENGTH_LONG).show();
            } else if (data.getData() != null) {
                String msg = acceptNsp(req, data.getData(), data.getFlags());
                if (msg != null) Toast.makeText(act, msg, Toast.LENGTH_LONG).show();
            }
        } catch (Exception e) {
            Toast.makeText(act, "Could not read that file: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
        refresh();
    }

    private String displayName(Uri uri) {
        try (android.database.Cursor c = act.getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                if (i >= 0 && c.getString(i) != null) return c.getString(i);
            }
        } catch (Exception ignored) {
        }
        return "file";
    }

    /** Returns an error message, or null when accepted. */
    private String acceptNsp(int req, Uri uri, int flags) throws IOException {
        String name = displayName(uri);
        String id = nspTitleId(uri);
        if (id == null) return name + " is not an NSP (no PFS0 header).";
        String want = req == REQ_BASE ? BASE_ID : UPDATE_ID;
        if (id.length() > 0 && !id.equals(want)) {
            if (id.equals(BASE_ID)) return name + " is the base game: choose it in the base game row.";
            if (id.equals(UPDATE_ID)) return name + " is the update: choose it in the update row.";
            return name + " is for a different title (" + id + ").";
        }
        try { // keep read access across restarts: the file is read in place when the game data is built
            act.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            return "Could not keep access to " + name + ". Pick it from a folder, not a recent-files shortcut.";
        }
        String k = req == REQ_BASE ? "base" : "update";
        prefs.edit().putString(k, uri.toString()).putString(k + "_name", name).apply();
        return id.isEmpty() ? name + " accepted, but its title could not be verified (no ticket inside)." : null;
    }

    /** The title id from the first ticket in the NSP's PFS0 header ("" if it has none), or null if it is no PFS0. */
    private String nspTitleId(Uri uri) throws IOException {
        try (InputStream raw = act.getContentResolver().openInputStream(uri);
             DataInputStream in = new DataInputStream(raw)) {
            byte[] h = new byte[16];
            in.readFully(h);
            ByteBuffer b = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
            if (b.getInt(0) != 0x30534650) return null; // "PFS0"
            int count = b.getInt(4), strSize = b.getInt(8);
            if (count < 1 || count > 4096 || strSize < 0 || strSize > (1 << 20)) return null;
            in.skipBytes(count * 24);
            byte[] st = new byte[strSize];
            in.readFully(st);
            String names = new String(st, StandardCharsets.ISO_8859_1);
            for (String n : names.split("\0")) {
                if (n.toLowerCase().endsWith(".tik") && n.length() >= 32) return n.substring(0, 16).toUpperCase();
            }
            return "";
        }
    }

    private String acceptKeys(Uri uri) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        try (InputStream in = act.getContentResolver().openInputStream(uri)) {
            byte[] buf = new byte[1 << 14];
            int n;
            while ((n = in.read(buf)) > 0) {
                bo.write(buf, 0, n);
                if (bo.size() > (2 << 20)) return displayName(uri) + " is too large to be a keys file.";
            }
        }
        String text = new String(bo.toByteArray(), StandardCharsets.UTF_8);
        boolean prod = text.toLowerCase().contains("master_key_00");
        boolean title = TITLE_KEY.matcher(text).find();
        if (!prod && !title) return displayName(uri) + " has no prod keys or title keys in it.";
        File dir = keysDir();
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        // a file with prod keys is prod.keys (it may carry title keys as well); otherwise it is title.keys
        try (FileOutputStream out = new FileOutputStream(new File(dir, prod ? "prod.keys" : "title.keys"))) {
            out.write(bo.toByteArray());
        }
        return null;
    }

    // ---- building the game data (extract/Extractor.java) ----

    private volatile boolean cancel;
    private boolean working;
    private android.widget.ProgressBar bar;
    private TextView stageText;
    private long lastUi;

    private void onContinue() {
        if (working) return;
        working = true;
        cancel = false;
        cont.setEnabled(false);
        cont.setAlpha(0.4f);
        later.setText("Cancel");
        later.setOnClickListener(v -> { cancel = true; later.setEnabled(false); });
        bar.setVisibility(View.VISIBLE);
        stageText.setVisibility(View.VISIBLE);
        stageText.setText("Starting...");
        act.getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); // a long job: keep the screen on
        new Thread(this::buildGameData, "extract").start();
    }

    private void buildGameData() {
        String error = null;
        final java.io.File home = act.getExternalFilesDir(null); // flags, saves and caches stay here
        Loc chosen = selectedLoc();
        java.io.File out = chosen != null ? chosen.dir : home; // exefs/ and romfs.bin go to the chosen volume
        java.util.List<java.io.Closeable> open = new java.util.ArrayList<>();
        try {
            if (out == null || home == null) throw new IOException("The app's storage folder is not available.");
            android.content.ContentResolver cr = act.getContentResolver();
            android.os.ParcelFileDescriptor pb = cr.openFileDescriptor(Uri.parse(saved("base")), "r");
            open.add(pb);
            android.os.ParcelFileDescriptor pu = cr.openFileDescriptor(Uri.parse(saved("update")), "r");
            open.add(pu);
            java.io.FileInputStream fb = new java.io.FileInputStream(pb.getFileDescriptor()), fu = new java.io.FileInputStream(pu.getFileDescriptor());
            open.add(fb);
            open.add(fu);
            java.util.List<InputStream> keyStreams = new java.util.ArrayList<>();
            for (String n : new String[] {"prod.keys", "title.keys"}) {
                File kf = new File(keysDir(), n);
                if (kf.isFile()) {
                    java.io.FileInputStream ks = new java.io.FileInputStream(kf);
                    open.add(ks);
                    keyStreams.add(ks);
                }
            }
            dev.teton.somniel.extract.Extractor.run(new dev.teton.somniel.extract.Source.Channel(fb.getChannel(), false),
                    new dev.teton.somniel.extract.Source.Channel(fu.getChannel(), false),
                    keyStreams.toArray(new InputStream[0]), out, new dev.teton.somniel.extract.Extractor.Progress() {
                        @Override public void update(String stage, long done, long total) { progress(stage, done, total); }
                        @Override public boolean cancelled() { return cancel; }
                    });
            // game_dir tells the native side where the game files are (removed when they are in the app's own folder)
            File gd = new File(home, "game_dir");
            if (out.equals(home)) gd.delete();
            else try (FileOutputStream w = new FileOutputStream(gd)) { w.write((out.getAbsolutePath() + "\n").getBytes("UTF-8")); }
            new FileOutputStream(new File(home, "spike.on")).close(); // tells the native side to run the game from this data
        } catch (Exception e) {
            error = cancel ? "Cancelled." : e.getMessage() != null ? e.getMessage() : e.toString();
            if (out != null) new File(out, "romfs.bin.part").delete(); // a partial file is worth nothing and is large
        } finally {
            for (java.io.Closeable c : open) try { c.close(); } catch (IOException ignored) { }
        }
        final String err = error;
        act.runOnUiThread(() -> finished(err));
    }

    private void progress(String stage, long done, long total) {
        long now = android.os.SystemClock.uptimeMillis();
        if (now - lastUi < 250 && done != total) return;
        lastUi = now;
        act.runOnUiThread(() -> {
            if (total > 0) {
                bar.setIndeterminate(false);
                bar.setMax(1000);
                bar.setProgress((int) (done * 1000 / total));
                stageText.setText(String.format(java.util.Locale.US, "%s: %.1f of %.1f GB", stage, done / 1e9, total / 1e9));
            } else {
                bar.setIndeterminate(true);
                stageText.setText(stage + "...");
            }
        });
    }

    private void finished(String error) {
        working = false;
        // on success the screen stays on until Start is pressed (the restart ends the process), so the device cannot lock
        // while "Game data ready" waits; after a failure the flag goes
        if (error != null) act.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        bar.setVisibility(View.GONE);
        stageText.setVisibility(View.GONE);
        later.setText("Not now");
        later.setEnabled(true);
        later.setOnClickListener(v -> hide());
        refresh();
        if (error != null) {
            new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Could not build the game data").setMessage(error).setPositiveButton("OK", null).show();
            return;
        }
        new AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Game data ready")
                .setMessage("The game restarts now. The first start also builds its shaders on this device, which takes "
                        + "a short while.")
                .setCancelable(false)
                .setPositiveButton("Start", (d, w) -> act.restartGame())
                .show();
    }

    // ---- views ----

    private int px(int dips) { return Math.round(dips * dp); }

    private LinearLayout.LayoutParams pad(int l, int t, int r, int b) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
        p.setMargins(px(l), px(t), px(r), px(b));
        return p;
    }

    private TextView text(String s, int sp, boolean bold, int color) {
        TextView t = new TextView(act);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private TextView status() { return text("", 13, false, 0xFFD2D8E8); }

    private StateListDrawable focusBg() {
        StateListDrawable d = new StateListDrawable();
        GradientDrawable hi = new GradientDrawable();
        hi.setColor(0xFF2F3B63);
        d.addState(new int[] {android.R.attr.state_focused}, hi);
        d.addState(new int[] {android.R.attr.state_pressed}, hi);
        d.addState(new int[] {}, new ColorDrawable(0xFF232A3D));
        return d;
    }

    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(act);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setBackground(focusBg());
        b.setOnClickListener(l);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, px(44));
        p.setMargins(px(8), 0, 0, 0);
        b.setLayoutParams(p);
        return b;
    }

    private View pickRow(String title, TextView status, View.OnClickListener l) {
        LinearLayout row = new LinearLayout(act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout left = new LinearLayout(act);
        left.setOrientation(LinearLayout.VERTICAL);
        left.addView(text(title, 16, false, Color.WHITE));
        left.addView(status);
        row.addView(left, new LinearLayout.LayoutParams(0, -2, 1f));
        row.addView(button("Choose...", l));
        row.setPadding(0, px(8), 0, px(8));
        return row;
    }

    private static String[] nspMimes() {
        return new String[] {"application/octet-stream", "application/x-nx-nsp", "*/*"};
    }
}
