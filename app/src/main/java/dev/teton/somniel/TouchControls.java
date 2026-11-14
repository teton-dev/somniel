package dev.teton.somniel;

import android.content.Context;
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
        float cx, cy, r, hw, hh; // circle: r; pill: half width / half height
        Btn(String label, long bit, boolean pill) { this.label = label; this.bit = bit; this.pill = pill; }
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

    private final Btn a = new Btn("A", A, false), b = new Btn("B", B, false), x = new Btn("X", X, false),
            y = new Btn("Y", Y, false), minus = new Btn("−", MINUS, false), plus = new Btn("+", PLUS, false),
            l = new Btn("L", L, true), zl = new Btn("ZL", ZL, true), r = new Btn("R", R, true),
            zr = new Btn("ZR", ZR, true);
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
    private final Runnable fade = () -> animate().alpha(FADED_ALPHA).setDuration(600).start();

    TouchControls(Context c) {
        super(c);
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
        if (getHeight() > 0) placeDpad();
        releaseAll();
        if (getVisibility() == VISIBLE) wake();
    }

    /** Full strength now; fades again after IDLE_MS (restarted by every touch). */
    private void wake() {
        handler.removeCallbacks(fade);
        animate().cancel();
        setAlpha(1f);
        if (fadeOn) handler.postDelayed(fade, IDLE_MS);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        float s = sc = Math.max(0.1f, Math.min(h / 360f, w / 800f));
        stickR[0] = 56 * s; knobR[0] = 24 * s;
        stickR[1] = 50 * s; knobR[1] = 22 * s;             // the camera stick is used less: a little smaller
        homeX[0] = 114 * s; homeY[0] = h - 132 * s;        // left stick, lower left
        homeX[1] = w - 263 * s; homeY[1] = h - 96 * s;     // right stick, left of the face buttons
        for (int i = 0; i < 2; i++) { cX[i] = homeX[i]; cY[i] = homeY[i]; }
        placeDpad();
        float fx = w - 127 * s, fy = h - 125 * s, o = 47 * s;
        place(a, fx + o, fy, 31 * s);                      // A: the most used, the biggest
        place(b, fx, fy + o, 27 * s);                      // B: next
        place(x, fx, fy - o, 23 * s);
        place(y, fx - o, fy, 23 * s);
        place(minus, w / 2f - 34 * s, 28 * s, 13 * s);     // top centre
        place(plus, w / 2f + 34 * s, 28 * s, 13 * s);
        pill(zl, 54 * s, 112 * s, 50 * s, 24 * s);         // shoulders: two wide keys side by side
        pill(l, 158 * s, 112 * s, 50 * s, 24 * s);
        pill(zr, w - 54 * s, 112 * s, 50 * s, 24 * s);
        pill(r, w - 158 * s, 112 * s, 50 * s, 24 * s);
        line.setStrokeWidth(Math.max(1.5f, 1.6f * s));
        guide.setStyle(Paint.Style.STROKE);
        guide.setStrokeWidth(Math.max(1f, 1.2f * s));
        guide.setPathEffect(new DashPathEffect(new float[] { 3 * s, 5 * s }, 0));
        chev.setStyle(Paint.Style.STROKE);
        chev.setStrokeCap(Paint.Cap.ROUND);
        chev.setStrokeJoin(Paint.Join.ROUND);
        chev.setStrokeWidth(Math.max(2f, 2.6f * s));
    }

    /** The D-pad sits left of centre; alone (no left stick) it is bigger and takes the stick's place. */
    private void placeDpad() {
        float s = sc;
        if (showLeftStick) { dpadX = 256 * s; dpadY = getHeight() - 84 * s; dpadR = 48 * s; }
        else { dpadX = homeX[0]; dpadY = homeY[0]; dpadR = 56 * s; }
    }

    private static void place(Btn b, float cx, float cy, float r) { b.cx = cx; b.cy = cy; b.r = r; }
    private static void pill(Btn b, float cx, float cy, float hw, float hh) { b.cx = cx; b.cy = cy; b.hw = hw; b.hh = hh; }

    // ---- touch ----

    @Override
    public boolean onTouchEvent(MotionEvent e) {
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
                body(t.cx, t.cy, t.hw * k, t.hh * k, 16 * s, on ? FILL_ON : FILL, on ? EDGE_ON : EDGE);
            } else {
                disc(t.cx, t.cy, t.r, on);
            }
            float size = t == a ? 19 : t == b ? 17 : t == minus || t == plus ? 18 : t.pill ? 14 : 16;
            label(t.label, t.cx, t.cy, on, size * s);
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
        canvas = null;
    }
}
