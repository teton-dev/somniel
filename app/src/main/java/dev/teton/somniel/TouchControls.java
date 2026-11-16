package dev.teton.somniel;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Transparent on-screen gamepad for phones. The layout follows a familiar Switch-emulator arrangement, with the infrequent
 * buttons kept out of the way and the most used button the biggest:
 *   lower left      : left stick (relative: it re-centres where the thumb lands), the game's walking and cursor stick
 *   left of centre  : D-pad (four chevron keys)
 *   right of centre : right stick (relative), the camera
 *   lower right     : A (largest), B, X, Y diamond; A and B are a little bigger than X and Y
 *   upper corners   : ZL, L and R, ZR side by side; top centre: Minus and Plus
 * L3 / R3 and a menu button are left out: the game has no use for them, and Back (the gesture) opens the sidebar.
 * Multi-touch: every finger is tracked on its own, a finger can slide from one button to the next, and a stick keeps the
 * finger that first touched it. The controls fade to FADED_ALPHA after IDLE_MS without a touch and come back at the next
 * touch. The state goes to the native input through nativeTouchPad; button bits match main.c's NP_* enum. The layout is
 * drawn in an 800 x 360 unit space scaled to fit the view.
 */
final class TouchControls extends View {
    static final long A = 1L << 0, B = 1L << 1, X = 1L << 2, Y = 1L << 3, L = 1L << 6, R = 1L << 7, ZL = 1L << 8,
            ZR = 1L << 9, PLUS = 1L << 10, MINUS = 1L << 11, LEFT = 1L << 12, UP = 1L << 13, RIGHT = 1L << 14,
            DOWN = 1L << 15;

    private static final int NONE = -1;
    private static final float DEAD = 0.12f;      // stick dead zone, fraction of the radius
    private static final long IDLE_MS = 4000;     // no touch for this long: fade
    private static final float FADED_ALPHA = 0.06f;

    /** A round or pill-shaped press target in view pixels. */
    private static final class Btn {
        final String label;
        final long bit;
        final boolean pill;
        final int group;
        float cx, cy, r, hw, hh; // circle: r; pill: half width / half height
        float gs = 1f;           // its group's scale (text and corners follow it)
        Btn(String label, long bit, boolean pill, int group) {
            this.label = label; this.bit = bit; this.pill = pill; this.group = group;
        }
        boolean hit(float x, float y, float grow) {
            if (pill) return Math.abs(x - cx) <= hw + grow && Math.abs(y - cy) <= hh + grow;
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= (r + grow) * (r + grow);
        }
        float dist2(float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy); }
    }

    interface Listener {
        void onPad(long buttons, int lx, int ly, int rx, int ry);
    }

    // groups (moved and resized as one in the layout editor)
    private static final int G_LSTICK = 0, G_DPAD = 1, G_RSTICK = 2, G_FACE = 3, G_CENTER = 4, G_LSHOULDER = 5,
            G_RSHOULDER = 6, GROUPS = 7;
    private static final String[] GROUP_NAMES = { "Left stick", "D-pad", "Right stick", "A B X Y buttons",
            "Minus and Plus", "ZL and L", "R and ZR" };
    static final int SIZE_MIN = 50, SIZE_MAX = 150, OPACITY_MIN = 20;
    private static final float GSC_MIN = 0.5f, GSC_MAX = 2f;

    private final Btn a = new Btn("A", A, false, G_FACE), b = new Btn("B", B, false, G_FACE),
            x = new Btn("X", X, false, G_FACE), y = new Btn("Y", Y, false, G_FACE),
            minus = new Btn("−", MINUS, false, G_CENTER), plus = new Btn("+", PLUS, false, G_CENTER),
            l = new Btn("L", L, true, G_LSHOULDER), zl = new Btn("ZL", ZL, true, G_LSHOULDER),
            r = new Btn("R", R, true, G_RSHOULDER), zr = new Btn("ZR", ZR, true, G_RSHOULDER);

    // saved layout: overall size / opacity (%), and per group an offset (fraction of the view) and a scale
    private int sizePct = 100, opacityPct = 100;
    private final float[] gOx = new float[GROUPS], gOy = new float[GROUPS], gSc = new float[GROUPS];
    private final SharedPreferences lp;
    // layout editor
    private boolean editing;
    private Runnable editDone;
    private int sel = -1, dragPtr = NONE;
    private float lastX, lastY;
    private final RectF[] tool = { new RectF(), new RectF(), new RectF(), new RectF() };
    private static final String[] TOOL_NAMES = { "Smaller", "Bigger", "Reset", "Done" };
    private final Btn[] btns = { a, b, x, y, minus, plus, l, zl, r, zr };

    // sticks: [0] left, [1] right. home = resting place, c = where the base is drawn now (relative sticks move it)
    private final float[] homeX = new float[2], homeY = new float[2], cX = new float[2], cY = new float[2];
    private final float[] stickR = new float[2], knobR = new float[2];
    private float dpadX, dpadY, dpadR; // dpadR = distance from the centre to each key
    private final int[] stickPtr = { NONE, NONE };
    private final float[] knobDx = new float[2], knobDy = new float[2];

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG), guide = new Paint(Paint.ANTI_ALIAS_FLAG),
            chev = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final Path path = new Path();
    private Canvas canvas;
    private float sc = 1f; // view pixels per layout unit

    private boolean showDpad = true, showLeftStick = true, fadeOn = true;
    private Listener listener;
    private long buttons, lastBits = -1;
    private int lx, ly, rx, ry;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable fade = () -> animate().alpha(Math.min(FADED_ALPHA, opacityPct / 100f)).setDuration(600).start();

    TouchControls(Context c) {
        super(c);
        lp = c.getSharedPreferences("touchlayout", Context.MODE_PRIVATE);
        sizePct = clampPct(lp.getInt("size", 100), SIZE_MIN, SIZE_MAX);
        opacityPct = clampPct(lp.getInt("opacity", 100), OPACITY_MIN, 100);
        for (int g = 0; g < GROUPS; g++) {
            gOx[g] = lp.getFloat("g" + g + "x", 0f);
            gOy[g] = lp.getFloat("g" + g + "y", 0f);
            gSc[g] = Math.max(GSC_MIN, Math.min(GSC_MAX, lp.getFloat("g" + g + "s", 1f)));
        }
        line.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setClickable(false);
        setFocusable(false);
        setVisibility(GONE);
    }

    void setListener(Listener l) { listener = l; }

    /** Which left-hand controls are drawn and whether the idle fade is on (sidebar). */
    void configure(boolean dpad, boolean leftStick, boolean fade) {
        showDpad = dpad;
        showLeftStick = leftStick;
        fadeOn = fade;
        relayout();
        releaseAll();
        if (getVisibility() == VISIBLE) wake();
    }

    private static int clampPct(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }

    // ---- layout settings (sidebar) ----

    int sizePct() { return sizePct; }
    int opacityPct() { return opacityPct; }

    void setSizePct(int v) {
        v = clampPct(v, SIZE_MIN, SIZE_MAX);
        if (v == sizePct) return;
        sizePct = v;
        lp.edit().putInt("size", v).commit();
        for (int g = 0; g < GROUPS; g++) clampGroup(g);
        relayout();
    }

    void setOpacityPct(int v) {
        v = clampPct(v, OPACITY_MIN, 100);
        if (v == opacityPct) return;
        opacityPct = v;
        lp.edit().putInt("opacity", v).commit();
        if (getVisibility() == VISIBLE && !editing) wake();
    }

    /** Every control back to its original place and size (overall size and opacity are kept). */
    void resetLayout() {
        for (int g = 0; g < GROUPS; g++) { gOx[g] = gOy[g] = 0f; gSc[g] = 1f; }
        saveLayout();
        relayout();
    }

    private void saveLayout() {
        SharedPreferences.Editor e = lp.edit();
        for (int g = 0; g < GROUPS; g++) {
            e.putFloat("g" + g + "x", gOx[g]).putFloat("g" + g + "y", gOy[g]).putFloat("g" + g + "s", gSc[g]);
        }
        e.commit();
    }

    // ---- layout editor ----

    boolean isEditing() { return editing; }

    /** Starts the editor: every control is shown at full strength; drag to move, tap one then Smaller / Bigger. */
    void startEdit(Runnable done) {
        editing = true;
        editDone = done;
        sel = -1;
        dragPtr = NONE;
        releaseAll();
        setVisibility(VISIBLE);
        handler.removeCallbacks(fade);
        animate().cancel();
        setAlpha(1f);
        relayout();
        invalidate();
    }

    void endEdit() {
        if (!editing) return;
        editing = false;
        sel = -1;
        saveLayout();
        invalidate();
        Runnable d = editDone;
        editDone = null;
        if (d != null) d.run(); // shows or hides the controls as the settings say, and restarts the idle fade
    }

    private boolean groupVisible(int g) {
        return g == G_LSTICK ? showLeftStick : g == G_DPAD ? showDpad : true;
    }

    /** Size of a group: the overall size times its own scale. */
    private float k(int g) { return sizePct / 100f * gSc[g]; }

    /** Places every control from the base layout, the overall size and the saved offsets / scales. */
    private void relayout() {
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;
        float s = sc = Math.max(0.1f, Math.min(h / 360f, w / 800f));
        float ox, oy, k;
        k = k(G_LSTICK); ox = gOx[G_LSTICK] * w; oy = gOy[G_LSTICK] * h;
        stickR[0] = 56 * s * k; knobR[0] = 24 * s * k;
        homeX[0] = 114 * s + ox; homeY[0] = h - 132 * s + oy;        // left stick, lower left
        k = k(G_RSTICK); ox = gOx[G_RSTICK] * w; oy = gOy[G_RSTICK] * h;
        stickR[1] = 50 * s * k; knobR[1] = 22 * s * k;               // the camera stick is used less: a little smaller
        homeX[1] = w - 263 * s + ox; homeY[1] = h - 96 * s + oy;     // right stick, left of the face buttons
        for (int i = 0; i < 2; i++) {
            if (stickPtr[i] == NONE) { cX[i] = homeX[i]; cY[i] = homeY[i]; }
        }
        k = k(G_DPAD); ox = gOx[G_DPAD] * w; oy = gOy[G_DPAD] * h;
        // the D-pad sits left of centre; alone (no left stick) it is bigger and takes the stick's place
        if (showLeftStick) { dpadX = 256 * s + ox; dpadY = h - 84 * s + oy; dpadR = 48 * s * k; }
        else { dpadX = 114 * s + ox; dpadY = h - 132 * s + oy; dpadR = 56 * s * k; }
        k = k(G_FACE); ox = gOx[G_FACE] * w; oy = gOy[G_FACE] * h;
        float fx = w - 127 * s + ox, fy = h - 125 * s + oy, o = 47 * s * k;
        place(a, fx + o, fy, 31 * s * k);                            // A: the most used, the biggest
        place(b, fx, fy + o, 27 * s * k);                            // B: next
        place(x, fx, fy - o, 23 * s * k);
        place(y, fx - o, fy, 23 * s * k);
        k = k(G_CENTER); ox = gOx[G_CENTER] * w; oy = gOy[G_CENTER] * h;
        place(minus, w / 2f - 34 * s * k + ox, 28 * s + oy, 13 * s * k); // top centre
        place(plus, w / 2f + 34 * s * k + ox, 28 * s + oy, 13 * s * k);
        // shoulders: two wide keys side by side, scaled about their pair's centre
        k = k(G_LSHOULDER); ox = gOx[G_LSHOULDER] * w; oy = gOy[G_LSHOULDER] * h;
        pill(zl, 106 * s - 52 * s * k + ox, 112 * s + oy, 50 * s * k, 24 * s * k);
        pill(l, 106 * s + 52 * s * k + ox, 112 * s + oy, 50 * s * k, 24 * s * k);
        k = k(G_RSHOULDER); ox = gOx[G_RSHOULDER] * w; oy = gOy[G_RSHOULDER] * h;
        pill(zr, w - 106 * s + 52 * s * k + ox, 112 * s + oy, 50 * s * k, 24 * s * k);
        pill(r, w - 106 * s - 52 * s * k + ox, 112 * s + oy, 50 * s * k, 24 * s * k);
        for (Btn t : btns) t.gs = k(t.group);
        // the editor's tool bar: a row of four keys in the middle of the view
        float tw = 46 * s, th = 20 * s, gap = 10 * s, x0 = w / 2f - (4 * 2 * tw + 3 * gap) / 2f;
        for (int i = 0; i < 4; i++) {
            float cx = x0 + tw + i * (2 * tw + gap);
            tool[i].set(cx - tw, h * 0.5f - th, cx + tw, h * 0.5f + th);
        }
        invalidate();
    }

    /** The screen area a group takes up (for the editor's outlines, hit tests and keeping it on screen). */
    private void bounds(int g, RectF out) {
        if (g == G_LSTICK || g == G_RSTICK) {
            int i = g == G_LSTICK ? 0 : 1;
            out.set(homeX[i] - stickR[i], homeY[i] - stickR[i], homeX[i] + stickR[i], homeY[i] + stickR[i]);
        } else if (g == G_DPAD) {
            float e = dpadR * 1.46f;
            out.set(dpadX - e, dpadY - e, dpadX + e, dpadY + e);
        } else {
            boolean first = true;
            for (Btn t : btns) {
                if (t.group != g) continue;
                float hw = t.pill ? t.hw : t.r, hh = t.pill ? t.hh : t.r;
                if (first) { out.set(t.cx - hw, t.cy - hh, t.cx + hw, t.cy + hh); first = false; }
                else out.union(t.cx - hw, t.cy - hh, t.cx + hw, t.cy + hh);
            }
        }
    }

    private final RectF tmp = new RectF();

    /** Nudges a group's offset so all of it stays inside the view. */
    private void clampGroup(int g) {
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;
        relayout();
        bounds(g, tmp);
        float dx = 0, dy = 0;
        if (tmp.left < 0) dx = -tmp.left; else if (tmp.right > w) dx = w - tmp.right;
        if (tmp.top < 0) dy = -tmp.top; else if (tmp.bottom > h) dy = h - tmp.bottom;
        if (tmp.width() > w) dx = w / 2f - tmp.centerX();   // wider than the view: centre it
        if (tmp.height() > h) dy = h / 2f - tmp.centerY();
        if (dx != 0 || dy != 0) { gOx[g] += dx / w; gOy[g] += dy / h; relayout(); }
    }

    /** Full strength now (the user's opacity); fades again after IDLE_MS (restarted by every touch). */
    private void wake() {
        handler.removeCallbacks(fade);
        animate().cancel();
        setAlpha(editing ? 1f : opacityPct / 100f);
        if (fadeOn && !editing) handler.postDelayed(fade, IDLE_MS);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        float s = sc = Math.max(0.1f, Math.min(h / 360f, w / 800f));
        for (int g = 0; g < GROUPS; g++) clampGroup(g);
        relayout();
        line.setStrokeWidth(Math.max(1.5f, 1.6f * s));
        guide.setStyle(Paint.Style.STROKE);
        guide.setStrokeWidth(Math.max(1f, 1.2f * s));
        guide.setPathEffect(new DashPathEffect(new float[] { 3 * s, 5 * s }, 0));
        chev.setStyle(Paint.Style.STROKE);
        chev.setStrokeCap(Paint.Cap.ROUND);
        chev.setStrokeJoin(Paint.Join.ROUND);
        chev.setStrokeWidth(Math.max(2f, 2.6f * s));
    }

    private static void place(Btn b, float cx, float cy, float r) { b.cx = cx; b.cy = cy; b.r = r; }
    private static void pill(Btn b, float cx, float cy, float hw, float hh) { b.cx = cx; b.cy = cy; b.hw = hw; b.hh = hh; }

    // ---- touch ----

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editing) return editTouch(e);
        int act = e.getActionMasked();
        int idx = e.getActionIndex();
        wake();
        if (act == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            return true;
        }
        boolean up = act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_POINTER_UP;
        // a new finger near a stick's home takes that stick (relative: the base moves to where it landed)
        if (act == MotionEvent.ACTION_DOWN || act == MotionEvent.ACTION_POINTER_DOWN) {
            int id = e.getPointerId(idx);
            float px = e.getX(idx), py = e.getY(idx);
            for (int i = showLeftStick ? 0 : 1; i < 2; i++) {
                float dx = px - homeX[i], dy = py - homeY[i], reach = stickR[i] * 1.6f;
                if (stickPtr[i] == NONE && dx * dx + dy * dy <= reach * reach && !onButton(px, py) && !onDpad(px, py)) {
                    stickPtr[i] = id;
                    cX[i] = Math.max(stickR[i], Math.min(getWidth() - stickR[i], px));
                    cY[i] = Math.max(stickR[i], Math.min(getHeight() - stickR[i], py));
                }
            }
        }
        long bits = 0;
        float[] ax = new float[4]; // lx ly rx ry in -1..1
        boolean[] seen = new boolean[2];
        for (int p = 0; p < e.getPointerCount(); p++) {
            if (up && p == idx) continue; // the finger that just lifted
            int id = e.getPointerId(p);
            float px = e.getX(p), py = e.getY(p);
            int s = stickPtr[0] == id ? 0 : stickPtr[1] == id ? 1 : NONE;
            if (s != NONE) {
                seen[s] = true;
                float dx = px - cX[s], dy = py - cY[s];
                float d = (float) Math.hypot(dx, dy);
                float k = d > stickR[s] ? stickR[s] / d : 1f;
                knobDx[s] = dx * k;
                knobDy[s] = dy * k;
                float nx = dx * k / stickR[s], ny = dy * k / stickR[s];
                if (Math.hypot(nx, ny) < DEAD) nx = ny = 0;
                ax[s * 2] = nx;
                ax[s * 2 + 1] = -ny; // up is positive
                continue;
            }
            bits |= buttonsAt(px, py);
        }
        for (int i = 0; i < 2; i++) {
            if (!seen[i]) { stickPtr[i] = NONE; knobDx[i] = knobDy[i] = 0; cX[i] = homeX[i]; cY[i] = homeY[i]; }
        }
        publish(bits, ax[0], ax[1], ax[2], ax[3]);
        invalidate();
        return true;
    }

    /** Editor: a tool-bar key acts at once; otherwise the finger picks a group and drags it. */
    private boolean editTouch(MotionEvent e) {
        int act = e.getActionMasked();
        int w = getWidth(), h = getHeight();
        if (act == MotionEvent.ACTION_DOWN) {
            float px = e.getX(), py = e.getY();
            dragPtr = NONE;
            for (int i = 0; i < 4; i++) {
                if (tool[i].contains(px, py)) { editTool(i); invalidate(); return true; }
            }
            sel = -1;
            float best = Float.MAX_VALUE, pad = 10 * sc;
            for (int g = 0; g < GROUPS; g++) {
                if (!groupVisible(g)) continue;
                bounds(g, tmp);
                tmp.inset(-pad, -pad);
                float area = tmp.width() * tmp.height();
                if (tmp.contains(px, py) && area < best) { best = area; sel = g; }
            }
            if (sel >= 0) { dragPtr = e.getPointerId(0); lastX = px; lastY = py; }
        } else if (act == MotionEvent.ACTION_MOVE && sel >= 0 && dragPtr != NONE && w > 0 && h > 0) {
            int i = e.findPointerIndex(dragPtr);
            if (i >= 0) {
                float px = e.getX(i), py = e.getY(i);
                gOx[sel] += (px - lastX) / w;
                gOy[sel] += (py - lastY) / h;
                lastX = px;
                lastY = py;
                clampGroup(sel);
                relayout();
            }
        } else if (act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_CANCEL) {
            if (dragPtr != NONE) saveLayout();
            dragPtr = NONE;
        }
        invalidate();
        return true;
    }

    private void editTool(int i) {
        if (i == 3) { endEdit(); return; }
        if (i == 2) { // Reset: the selected control, or everything when none is selected
            if (sel >= 0) { gOx[sel] = gOy[sel] = 0f; gSc[sel] = 1f; saveLayout(); relayout(); }
            else resetLayout();
            return;
        }
        if (sel < 0) return;
        gSc[sel] = Math.max(GSC_MIN, Math.min(GSC_MAX, Math.round((gSc[sel] + (i == 1 ? 0.1f : -0.1f)) * 10) / 10f));
        clampGroup(sel);
        saveLayout();
        relayout();
    }

    private boolean onDpad(float px, float py) {
        return showDpad && Math.hypot(px - dpadX, py - dpadY) <= dpadR * 1.5f;
    }

    private boolean onButton(float px, float py) {
        for (Btn t : btns) if (t.hit(px, py, t.pill ? 6 : t.r * 0.25f)) return true;
        return false;
    }

    /** The buttons under a finger that is not holding a stick: face/shoulder/centre buttons and the D-pad. */
    private long buttonsAt(float px, float py) {
        long bits = 0;
        Btn best = null;
        float bd = Float.MAX_VALUE;
        for (Btn t : btns) {
            if (!t.hit(px, py, t.pill ? 6 : t.r * 0.25f)) continue;
            float d = t.dist2(px, py);
            if (d < bd) { bd = d; best = t; }
        }
        if (best != null) return best.bit;
        if (!showDpad) return bits;
        float dx = px - dpadX, dy = py - dpadY;
        float d = (float) Math.hypot(dx, dy);
        if (d <= dpadR * 1.5f && d > dpadR * 0.2f) {
            // 4-way: only the dominant axis counts (this game has no use for diagonals), so a thumb that drifts off
            // a key never produces two directions at once
            if (Math.abs(dx) >= Math.abs(dy)) bits |= dx < 0 ? LEFT : RIGHT;
            else bits |= dy < 0 ? UP : DOWN;
        }
        return bits;
    }

    private void publish(long bits, float lxs, float lys, float rxs, float rys) {
        int nlx = Math.round(lxs * 32767), nly = Math.round(lys * 32767), nrx = Math.round(rxs * 32767),
                nry = Math.round(rys * 32767);
        if ((bits & ~lastBits) != 0) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        lastBits = bits;
        if (bits == buttons && nlx == lx && nly == ly && nrx == rx && nry == ry) return;
        buttons = bits; lx = nlx; ly = nly; rx = nrx; ry = nry;
        if (listener != null) listener.onPad(buttons, lx, ly, rx, ry);
    }

    /** Lets go of everything (Cancel, or the controls were hidden). */
    void releaseAll() {
        stickPtr[0] = stickPtr[1] = NONE;
        for (int i = 0; i < 2; i++) { knobDx[i] = knobDy[i] = 0; cX[i] = homeX[i]; cY[i] = homeY[i]; }
        publish(0, 0, 0, 0, 0);
        invalidate();
    }

    @Override
    protected void onVisibilityChanged(View v, int vis) {
        super.onVisibilityChanged(v, vis);
        if (vis != VISIBLE) { releaseAll(); handler.removeCallbacks(fade); }
        else wake(); // shown (boot, or switched on in the sidebar): strong for a few seconds, then fades
    }

    // ---- drawing ----

    private static final int FILL = 0x52141A20, FILL_ON = 0x88FFFFFF, EDGE = 0xB0FFFFFF, EDGE_ON = 0xFFFFFFFF,
            SHADOW = 0x40000000;

    /** A rounded body (a circle when hw == hh == corner): a dark halo for contrast, the fill, a thin light edge. */
    private void body(float cx, float cy, float hw, float hh, float corner, int fillCol, int edgeCol) {
        rect.set(cx - hw, cy - hh, cx + hw, cy + hh);
        float w = line.getStrokeWidth();
        line.setStrokeWidth(w + 2.2f * sc);
        line.setColor(SHADOW);
        canvas.drawRoundRect(rect, corner, corner, line);
        line.setStrokeWidth(w);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(fillCol);
        canvas.drawRoundRect(rect, corner, corner, fill);
        line.setColor(edgeCol);
        canvas.drawRoundRect(rect, corner, corner, line);
    }

    private void disc(float cx, float cy, float rad, boolean on) {
        if (on) rad *= 1.06f;
        body(cx, cy, rad, rad, rad, on ? FILL_ON : FILL, on ? EDGE_ON : EDGE);
    }

    private void label(String s, float cx, float cy, boolean on, float size) {
        text.setTextSize(size);
        text.setColor(on ? 0xFFFFFFFF : 0xE6FFFFFF);
        text.setShadowLayer(2.5f * sc, 0, sc, 0x99000000);
        canvas.drawText(s, cx, cy - (text.ascent() + text.descent()) / 2f, text);
    }

    @Override
    protected void onDraw(Canvas c) {
        canvas = c;
        float s = sc;
        for (Btn t : btns) {
            boolean on = (buttons & t.bit) != 0;
            if (t.pill) {
                float k = on ? 1.04f : 1f;
                body(t.cx, t.cy, t.hw * k, t.hh * k, 16 * s * t.gs, on ? FILL_ON : FILL, on ? EDGE_ON : EDGE);
            } else {
                disc(t.cx, t.cy, t.r, on);
            }
            float size = t == a ? 19 : t == b ? 17 : t == minus || t == plus ? 18 : t.pill ? 14 : 16;
            label(t.label, t.cx, t.cy, on, size * s * t.gs);
        }
        // D-pad: four rounded keys with chevrons
        long[] dirs = { UP, DOWN, LEFT, RIGHT };
        float half = dpadR * 0.46f, k = dpadR * 0.2f;
        for (int i = 0; showDpad && i < 4; i++) {
            boolean on = (buttons & dirs[i]) != 0;
            float ox = i == 2 ? -1 : i == 3 ? 1 : 0, oy = i == 0 ? -1 : i == 1 ? 1 : 0;
            float cx = dpadX + ox * dpadR, cy = dpadY + oy * dpadR, hs = on ? half * 1.06f : half;
            body(cx, cy, hs, hs, hs * 0.38f, on ? FILL_ON : FILL, on ? EDGE_ON : EDGE);
            chev.setColor(on ? 0xFFFFFFFF : 0xE6FFFFFF);
            path.rewind();
            path.moveTo(cx - ox * k * 0.45f - oy * k, cy - oy * k * 0.45f + ox * k);
            path.lineTo(cx + ox * k * 0.55f, cy + oy * k * 0.55f);
            path.lineTo(cx - ox * k * 0.45f + oy * k, cy - oy * k * 0.45f - ox * k);
            c.drawPath(path, chev);
        }
        // sticks: base ring with a dashed guide (moves to where a thumb landed) plus a knob that follows the finger
        for (int i = showLeftStick ? 0 : 1; i < 2; i++) {
            body(cX[i], cY[i], stickR[i], stickR[i], stickR[i], 0x26101418, 0x78FFFFFF);
            guide.setColor(0x50FFFFFF);
            c.drawCircle(cX[i], cY[i], stickR[i] * 0.74f, guide);
            float kx = cX[i] + knobDx[i], ky = cY[i] + knobDy[i];
            boolean on = stickPtr[i] != NONE;
            disc(kx, ky, knobR[i], on);
            guide.setColor(on ? 0xA0FFFFFF : 0x60FFFFFF);
            c.drawCircle(kx, ky, knobR[i] * 0.58f, guide);
        }
        if (editing) drawEditor(c);
        canvas = null;
    }

    /** Editor overlay: a dashed outline around every control (the picked one in yellow) and the tool bar. */
    private void drawEditor(Canvas c) {
        float s = sc;
        fill.setStyle(Paint.Style.FILL);
        for (int g = 0; g < GROUPS; g++) {
            if (!groupVisible(g)) continue;
            bounds(g, rect);
            rect.inset(-6 * s, -6 * s);
            guide.setColor(g == sel ? 0xFFFFD84A : 0x90FFFFFF);
            guide.setStrokeWidth(Math.max(1.5f, (g == sel ? 2.4f : 1.4f) * s));
            c.drawRoundRect(rect, 10 * s, 10 * s, guide);
        }
        guide.setStrokeWidth(Math.max(1f, 1.2f * s));
        String hint = sel < 0 ? "Drag a control to move it. Tap one to resize it."
                : GROUP_NAMES[sel] + ": " + Math.round(k(sel) * 100) + "%  (drag to move)";
        text.setTextSize(15 * s);
        text.setColor(0xFFFFFFFF);
        text.setShadowLayer(3 * s, 0, s, 0xCC000000);
        c.drawText(hint, getWidth() / 2f, tool[0].top - 22 * s, text);
        for (int i = 0; i < 4; i++) {
            boolean dim = i < 2 && sel < 0;
            fill.setColor(dim ? 0x99202830 : 0xDD2F3B63);
            c.drawRoundRect(tool[i], 14 * s, 14 * s, fill);
            line.setColor(dim ? 0x60FFFFFF : 0xE0FFFFFF);
            c.drawRoundRect(tool[i], 14 * s, 14 * s, line);
            text.setColor(dim ? 0x80FFFFFF : 0xFFFFFFFF);
            text.setTextSize(14 * s);
            c.drawText(TOOL_NAMES[i], tool[i].centerX(), tool[i].centerY() - (text.ascent() + text.descent()) / 2f, text);
        }
    }
}
