package dev.teton.somniel;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Transparent on-screen gamepad for phones. The layout follows Microsoft's touch-layout guidance (primary actions under
 * the thumbs in the lower corners, infrequent buttons in the upper zone, the lower centre left empty, the most used
 * button the biggest, forgiving relative sticks):
 *   lower left  : left stick (relative: it re-centres where the thumb lands), the game's walking and cursor stick
 *   left, middle: D-pad
 *   lower right : A (largest), B, X, Y diamond
 *   right, middle: right stick (relative), the camera
 *   top corners : L / ZL and R / ZR; top centre: Minus and Plus
 * Multi-touch: every finger is tracked on its own, a finger can slide from one button to the next, and a stick keeps the
 * finger that first touched it. The controls fade to FADED_ALPHA after IDLE_MS without a touch and come back at the next
 * touch. Back (the gesture) still opens the sidebar. The state goes to the native input through nativeTouchPad; button
 * bits match main.c's NP_* enum. The layout is drawn in a 360-unit-high space scaled to the view's height.
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
    private float dpadX, dpadY, dpadR;
    private final int[] stickPtr = { NONE, NONE };
    private final float[] knobDx = new float[2], knobDy = new float[2];

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private Canvas canvas;

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
        float s = h / 360f;
        stickR[0] = 52 * s; knobR[0] = 22 * s;
        stickR[1] = 36 * s; knobR[1] = 16 * s;             // the camera stick is used less: smaller
        homeX[0] = 118 * s; homeY[0] = h - 90 * s;         // left stick, lower left, a little in from the edge
        homeX[1] = w - 88 * s; homeY[1] = 130 * s;         // right stick, above the face buttons
        for (int i = 0; i < 2; i++) { cX[i] = homeX[i]; cY[i] = homeY[i]; }
        placeDpad();
        float fx = w - 92 * s, fy = h - 92 * s, o = 40 * s;
        place(a, fx + o, fy, 26 * s);                      // A: the most used, the biggest
        place(b, fx, fy + o, 22 * s);
        place(x, fx, fy - o, 20 * s);
        place(y, fx - o, fy, 20 * s);
        place(minus, w / 2f - 46 * s, h - 28 * s, 15 * s);   // bottom centre
        place(plus, w / 2f + 46 * s, h - 28 * s, 15 * s);
        pill(zl, 58 * s, 24 * s, 38 * s, 15 * s);
        pill(l, 58 * s, 58 * s, 38 * s, 15 * s);
        pill(zr, w - 58 * s, 24 * s, 38 * s, 15 * s);
        pill(r, w - 58 * s, 58 * s, 38 * s, 15 * s);
        line.setStrokeWidth(Math.max(2f, 1.8f * s));
    }

    /** The D-pad sits left and dropped when the left stick is shown too; alone it is bigger and takes the stick's place. */
    private void placeDpad() {
        float s = getHeight() / 360f;
        if (showLeftStick) { dpadX = 80 * s; dpadY = 178 * s; dpadR = 42 * s; }
        else { dpadX = homeX[0]; dpadY = homeY[0]; dpadR = 60 * s; }
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
        return showDpad && Math.hypot(px - dpadX, py - dpadY) <= dpadR * 1.4f;
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
        if (d <= dpadR * 1.4f && d > dpadR * 0.2f) {
            // 4-way: only the dominant axis counts (this game has no use for diagonals), so a thumb that drifts off
            // an arm never produces two directions at once
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

    private static final int IDLE = 0x40303030, ON_FILL = 0x66FFFFFF, EDGE = 0x80FFFFFF, EDGE_ON = 0xCCFFFFFF;

    private void disc(float cx, float cy, float rad, boolean on) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(on ? ON_FILL : IDLE);
        line.setColor(on ? EDGE_ON : EDGE);
        canvas.drawCircle(cx, cy, rad, fill);
        canvas.drawCircle(cx, cy, rad, line);
    }

    private void label(String s, float cx, float cy, boolean on, float size) {
        text.setTextSize(size);
        text.setColor(on ? 0xFFFFFFFF : 0xCCFFFFFF);
        canvas.drawText(s, cx, cy - (text.ascent() + text.descent()) / 2f, text);
    }

    @Override
    protected void onDraw(Canvas c) {
        canvas = c;
        float s = getHeight() / 360f;
        for (Btn t : btns) {
            boolean on = (buttons & t.bit) != 0;
            if (t.pill) {
                rect.set(t.cx - t.hw, t.cy - t.hh, t.cx + t.hw, t.cy + t.hh);
                fill.setStyle(Paint.Style.FILL);
                fill.setColor(on ? ON_FILL : IDLE);
                line.setColor(on ? EDGE_ON : EDGE);
                c.drawRoundRect(rect, t.hh, t.hh, fill);
                c.drawRoundRect(rect, t.hh, t.hh, line);
            } else {
                disc(t.cx, t.cy, t.r, on);
            }
            label(t.label, t.cx, t.cy, on, (t == a ? 17 : 14) * s);
        }
        // D-pad: a plus of four arms
        float arm = dpadR * 0.36f;
        long[] dirs = { UP, DOWN, LEFT, RIGHT };
        for (int i = 0; showDpad && i < 4; i++) {
            boolean on = (buttons & dirs[i]) != 0;
            float ox = i == 2 ? -1 : i == 3 ? 1 : 0, oy = i == 0 ? -1 : i == 1 ? 1 : 0;
            float cx = dpadX + ox * dpadR * 0.62f, cy = dpadY + oy * dpadR * 0.62f;
            rect.set(cx - (ox != 0 ? dpadR * 0.38f : arm), cy - (oy != 0 ? dpadR * 0.38f : arm),
                    cx + (ox != 0 ? dpadR * 0.38f : arm), cy + (oy != 0 ? dpadR * 0.38f : arm));
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(on ? ON_FILL : IDLE);
            line.setColor(on ? EDGE_ON : EDGE);
            c.drawRoundRect(rect, arm * 0.5f, arm * 0.5f, fill);
            c.drawRoundRect(rect, arm * 0.5f, arm * 0.5f, line);
        }
        // sticks: base ring (moves to where a thumb landed) plus a knob that follows the finger
        for (int i = showLeftStick ? 0 : 1; i < 2; i++) {
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(0x22000000);
            line.setColor(0x66FFFFFF);
            c.drawCircle(cX[i], cY[i], stickR[i], fill);
            c.drawCircle(cX[i], cY[i], stickR[i], line);
            disc(cX[i] + knobDx[i], cY[i] + knobDy[i], knobR[i], stickPtr[i] != NONE);
        }
        canvas = null;
    }
}
