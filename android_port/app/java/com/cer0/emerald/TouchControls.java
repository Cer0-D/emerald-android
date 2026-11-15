package com.cer0.emerald;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.DisplayCutout;
import android.view.WindowInsets;

/*
 * The GBA's buttons drawn over the top screen, for a device without a
 * controller (MainActivity shows them while no gamepad is connected): the
 * d-pad on the left, A and B on the right, L and R in the top corners,
 * Select and Start in the bottom ones. Every finger is followed: sliding
 * from one button onto another moves the press; the d-pad gives one of four
 * directions, no diagonals. Presses go to the game as the keys of a controller
 * (MainActivity.nativeKey), so they behave exactly like one.
 */
final class TouchControls extends View {
    private static final int UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3, A = 4, B = 5, L = 6, R = 7,
            START = 8, SELECT = 9, COUNT = 10;
    private static final int[] KEYS = {
        KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT,
        KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
        KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_START,
        KeyEvent.KEYCODE_BUTTON_SELECT,
    };
    private static final String[] LABELS = {"", "", "", "", "A", "B", "L", "R", "START", "SELECT"};

    /* Laid out in onSizeChanged: the d-pad's centre and radius, the round
     * buttons' centres and radius, the bars' rectangles. */
    private float mPadX, mPadY, mPadR;
    private final float[] mRoundX = new float[COUNT], mRoundY = new float[COUNT];
    private float mRoundR;
    private final RectF[] mBar = new RectF[COUNT];
    private final boolean[] mDown = new boolean[COUNT];
    private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mLine = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mTmp = new RectF();

    TouchControls(Context c) {
        super(c);
        mLine.setStyle(Paint.Style.STROKE);
        mText.setTextAlign(Paint.Align.CENTER);
        mText.setFakeBoldText(true);
    }

    /* Kept clear on both sides: a camera hole or notch on either edge (the
     * larger of the two safe insets, so the layout stays symmetric), and
     * the same at the top and bottom. Follows the phone being turned over. */
    private int mSafeX, mSafeY;

    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        DisplayCutout cut = insets.getDisplayCutout();
        int x = cut == null ? 0 : Math.max(cut.getSafeInsetLeft(), cut.getSafeInsetRight());
        int y = cut == null ? 0 : Math.max(cut.getSafeInsetTop(), cut.getSafeInsetBottom());
        if (x != mSafeX || y != mSafeY) {
            mSafeX = x;
            mSafeY = y;
            layout(getWidth(), getHeight());
            invalidate();
        }
        return super.onApplyWindowInsets(insets);
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) { layout(w, h); }

    private void layout(int fullW, int fullH) {
        if (fullW <= 0 || fullH <= 0) return;
        // The buttons are laid out in the safe area, offset into place below.
        int w = fullW - 2 * mSafeX, h = fullH - 2 * mSafeY;
        // Sized by the screen's height, the hand's reach in landscape.
        float m = h * 0.07f;
        mPadR = h * 0.17f;
        mPadX = m + mPadR;
        mPadY = h * 0.58f;
        mRoundR = h * 0.085f;
        mRoundX[A] = w - m - mRoundR;
        mRoundY[A] = mPadY - mRoundR * 0.7f;
        mRoundX[B] = mRoundX[A] - mRoundR * 2.4f;
        mRoundY[B] = mPadY + mRoundR * 0.7f;
        float bw = h * 0.26f, bh = h * 0.1f, top = h * 0.04f;
        mBar[L] = new RectF(m * 0.6f, top, m * 0.6f + bw, top + bh);
        mBar[R] = new RectF(w - m * 0.6f - bw, top, w - m * 0.6f, top + bh);
        float sw = h * 0.2f, sh = h * 0.075f, bottom = h - h * 0.05f;
        // Select and Start mirror each other, a third of the way in from
        // under the d-pad towards the middle.
        float selX = mPadX + (w / 2f - mPadX) * 0.3f, startX = w - selX;
        mBar[SELECT] = new RectF(selX - sw / 2, bottom - sh, selX + sw / 2, bottom);
        mBar[START] = new RectF(startX - sw / 2, bottom - sh, startX + sw / 2, bottom);
        mLine.setStrokeWidth(Math.max(2f, h * 0.004f));
        mPadX += mSafeX;
        mPadY += mSafeY;
        for (int b = 0; b < COUNT; b++) {
            mRoundX[b] += mSafeX;
            mRoundY[b] += mSafeY;
            if (mBar[b] != null) mBar[b].offset(mSafeX, mSafeY);
        }
    }

    /* The buttons a finger at (x, y) presses, as bits. Touch areas reach a
     * little past what is drawn, so a thumb slightly off still lands. */
    private int hit(float x, float y) {
        float dx = x - mPadX, dy = y - mPadY, d = (float) Math.hypot(dx, dy);
        if (d <= mPadR * 1.35f) {
            if (d < mPadR * 0.2f) return 0;
            // Four directions, no diagonals: the one nearest the finger.
            if (Math.abs(dx) >= Math.abs(dy)) return 1 << (dx > 0 ? RIGHT : LEFT);
            return 1 << (dy > 0 ? DOWN : UP);
        }
        for (int b : new int[] {A, B})
            if (Math.hypot(x - mRoundX[b], y - mRoundY[b]) <= mRoundR * 1.3f) return 1 << b;
        for (int b : new int[] {L, R, START, SELECT}) {
            RectF r = mBar[b];
            float gx = r.height() * 0.4f, gy = r.height() * 0.5f;
            if (x >= r.left - gx && x <= r.right + gx && y >= r.top - gy && y <= r.bottom + gy) return 1 << b;
        }
        return 0;
    }

    /* The finger that went down on no button: the top screen's touch (the
     * bottom screen's window on a single screen), followed until let go. */
    private int mFree = -1;

    @Override public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked(), lifted = -1;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
            lifted = e.getActionIndex();
        if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) && mFree < 0) {
            int i = e.getActionIndex();
            if (hit(e.getX(i), e.getY(i)) == 0) {
                mFree = e.getPointerId(i);
                MainActivity.nativeTouchTop(0, e.getX(i), e.getY(i), getWidth(), getHeight());
            }
        }
        int held = 0;
        if (action != MotionEvent.ACTION_CANCEL)
            for (int i = 0; i < e.getPointerCount(); i++)
                if (i != lifted && e.getPointerId(i) != mFree) held |= hit(e.getX(i), e.getY(i));
        if (mFree >= 0) {
            int i = e.findPointerIndex(mFree);
            boolean up = action == MotionEvent.ACTION_CANCEL || i < 0 || i == lifted;
            if (up) {
                float x = i >= 0 ? e.getX(i) : 0, y = i >= 0 ? e.getY(i) : 0;
                MainActivity.nativeTouchTop(action == MotionEvent.ACTION_CANCEL ? 3 : 1, x, y, getWidth(), getHeight());
                mFree = -1;
            } else if (action == MotionEvent.ACTION_MOVE) {
                MainActivity.nativeTouchTop(2, e.getX(i), e.getY(i), getWidth(), getHeight());
            }
        }
        apply(held);
        return true;
    }

    /* Hand the game every change: releases, then presses. */
    private void apply(int held) {
        boolean pressed = false, changed = false;
        for (int b = 0; b < COUNT; b++) {
            boolean now = (held >> b & 1) != 0;
            if (now == mDown[b] || now) continue;
            mDown[b] = false;
            MainActivity.nativeKey(KEYS[b], false);
            changed = true;
        }
        for (int b = 0; b < COUNT; b++) {
            boolean now = (held >> b & 1) != 0;
            if (!now || mDown[b]) continue;
            mDown[b] = true;
            MainActivity.nativeKey(KEYS[b], true);
            changed = pressed = true;
        }
        if (pressed) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (changed) invalidate();
    }

    /* Hidden or the app leaving: nothing may stay held. */
    void releaseAll() { apply(0); }

    private void style(boolean down) {
        mFill.setColor(down ? 0x80FFFFFF : 0x30FFFFFF);
        mLine.setColor(down ? 0xC0FFFFFF : 0x70FFFFFF);
        mText.setColor(down ? 0xFF202020 : 0xB0FFFFFF);
    }

    @Override protected void onDraw(Canvas c) {
        // The d-pad: a cross over a faint disc, the held arm lit.
        float half = mPadR * 0.32f, reach = mPadR * 0.92f, corner = half * 0.35f;
        style(false);
        c.drawCircle(mPadX, mPadY, mPadR, mFill);
        mTmp.set(mPadX - reach, mPadY - half, mPadX + reach, mPadY + half);
        c.drawRoundRect(mTmp, corner, corner, mFill);
        c.drawRoundRect(mTmp, corner, corner, mLine);
        mTmp.set(mPadX - half, mPadY - reach, mPadX + half, mPadY + reach);
        c.drawRoundRect(mTmp, corner, corner, mFill);
        c.drawRoundRect(mTmp, corner, corner, mLine);
        int[][] arms = {{UP, 0, -1}, {DOWN, 0, 1}, {LEFT, -1, 0}, {RIGHT, 1, 0}};
        for (int[] a : arms) {
            if (!mDown[a[0]]) continue;
            float x0 = mPadX + (a[1] != 0 ? Math.min(a[1] * half, a[1] * reach) : -half);
            float x1 = mPadX + (a[1] != 0 ? Math.max(a[1] * half, a[1] * reach) : half);
            float y0 = mPadY + (a[2] != 0 ? Math.min(a[2] * half, a[2] * reach) : -half);
            float y1 = mPadY + (a[2] != 0 ? Math.max(a[2] * half, a[2] * reach) : half);
            mTmp.set(x0, y0, x1, y1);
            style(true);
            c.drawRoundRect(mTmp, corner, corner, mFill);
        }
        for (int b : new int[] {A, B}) {
            style(mDown[b]);
            c.drawCircle(mRoundX[b], mRoundY[b], mRoundR, mFill);
            c.drawCircle(mRoundX[b], mRoundY[b], mRoundR, mLine);
            mText.setTextSize(mRoundR * 0.9f);
            c.drawText(LABELS[b], mRoundX[b], mRoundY[b] + mRoundR * 0.32f, mText);
        }
        for (int b : new int[] {L, R, START, SELECT}) {
            RectF r = mBar[b];
            float rad = r.height() / 2;
            style(mDown[b]);
            c.drawRoundRect(r, rad, rad, mFill);
            c.drawRoundRect(r, rad, rad, mLine);
            mText.setTextSize(r.height() * (b == L || b == R ? 0.55f : 0.42f));
            c.drawText(LABELS[b], r.centerX(), r.centerY() + mText.getTextSize() * 0.36f, mText);
        }
    }
}
