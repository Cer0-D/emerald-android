package com.cer0.emerald;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/**
 * Hosts the two screens: the top (game) screen is this activity's SurfaceView on
 * the default display; the bottom (touch UI) screen is a Presentation on the
 * Thor's second display. Both surfaces go to native code, which owns one EGL
 * context and draws them from its own thread.
 */
public class MainActivity extends Activity {
    static final String TAG = "Emerald";
    // The boot library (android_port/boot/emerald_boot.c) loads one
    // language's game library, libemerald_<code>.so, below 4 GiB on request
    // (nativeLoadGame), and that one registers the methods below.
    static { System.loadLibrary("emeraldboot"); }

    static native boolean nativeLoadGame(String code);

    static native void nativeInit(String filesDir, String romfsDir);
    static native void nativeSurface(int screen, Object surface); // surface null = gone
    static native void nativeKey(int keyCode, boolean down);
    static native void nativeAxis(float x, float y);
    static native void nativeTouch(int action, float x, float y, int w, int h);
    static native void nativePause(boolean paused);
    static native void nativeCapture();

    public static final int SCREEN_TOP = 0, SCREEN_BOTTOM = 1;

    private BottomPresentation mBottom;
    /* The game runs (nativeInit): until then the first-boot setup has the
     * screen (Setup), and nothing native is touched but the library. */
    private boolean mStarted;
    private Setup mSetup;
    private boolean mResumed;
    static final int REQUEST_ROM = 1;

    final class ScreenCallback implements SurfaceHolder.Callback {
        final int screen;
        ScreenCallback(int s) { screen = s; }
        @Override public void surfaceCreated(SurfaceHolder h) {}
        @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {
            Log.i(TAG, "surface " + screen + " " + w + "x" + hh);
            nativeSurface(screen, h.getSurface());
        }
        @Override public void surfaceDestroyed(SurfaceHolder h) { nativeSurface(screen, null); }
    }

    final class BottomPresentation extends Presentation {
        BottomPresentation(Context c, Display d) { super(c, d); }
        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            SurfaceView v = new SurfaceView(getContext());
            v.getHolder().addCallback(new ScreenCallback(SCREEN_BOTTOM));
            v.setOnTouchListener((view, e) -> {
                int a = e.getActionMasked();
                nativeTouch(a, e.getX(), e.getY(), view.getWidth(), view.getHeight());
                return true;
            });
            setContentView(v);
            hideBars(getWindow().getDecorView());
        }
        // Buttons pressed while the presentation has focus still drive the game.
        @Override public boolean dispatchKeyEvent(KeyEvent e) {
            return MainActivity.this.handleKey(e) || super.dispatchKeyEvent(e);
        }
        @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
            return MainActivity.this.handleMotion(e) || super.dispatchGenericMotionEvent(e);
        }
    }

    static void hideBars(View v) {
        WindowInsetsController c = v.getWindowInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    static java.io.File filesDir(Context c) {
        java.io.File dir = c.getExternalFilesDir(null);
        return dir != null ? dir : c.getFilesDir();
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        hideBars(getWindow().getDecorView());
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Local wireless play's radio (Bluetooth LE) asks through this activity.
        LocalBluetooth.attach(this);
        /*
         * The APK carries every supported language (Languages); each plays
         * from its own root. The one played last starts at once when its data
         * pack was built for this APK; otherwise the setup screen asks for a
         * ROM, and the ROM's language is the one built and played (Setup).
         * A development install takes its ROMs from <files>/roms/ instead,
         * the one named by `--es rom NAME` first (tools/dev_rom.sh).
         */
        Languages.migrate(this);
        String code = Languages.active(this);
        String devRom = Languages.isDev(this) ? getIntent().getStringExtra("rom") : null;
        if (devRom == null && Languages.carries(this, code) && Languages.packReady(this, code)) {
            startGame(code);
            return;
        }
        mSetup = new Setup(this);
        mSetup.show();
        if (Languages.isDev(this))
            mSetup.devRom(devRom, Languages.carries(this, code) ? code : null);
    }

    /* A language's game: its library loaded, its engine files extracted,
     * and its root - engine/ (romfs:/) and sdmc/ (save, settings, log, data
     * pack, follower files), in the app's external files dir: reachable over
     * adb, removed with the app - handed to it. */
    void startGame(String code) {
        if (mStarted) return;
        if (!nativeLoadGame(code)) {
            // Another language's library is in this process: a fresh process
            // for this one. Once - a library that does not load at all says so.
            if (!getIntent().getBooleanExtra("restarted", false)) {
                Log.w(TAG, "restarting for " + code);
                restart();
                return;
            }
            android.widget.TextView t = new android.widget.TextView(this);
            t.setText("The game library for \"" + code + "\" did not load (see logcat).");
            t.setTextSize(20);
            setContentView(t);
            return;
        }
        try {
            Setup.extractEngine(this, code);
        } catch (java.io.IOException e) {
            Log.e(TAG, "engine files", e);
        }
        // The follower Pokémon's sprites and texts, downloaded in the
        // background until they are all there (FollowerFetcher).
        FollowerFetcher.start(this, code);
        mStarted = true;
        mSetup = null;
        Log.i(TAG, "playing " + code);
        nativeInit(Languages.root(this, code).getAbsolutePath(), Setup.ENGINE_DIR);
        handleIntent(getIntent());
        SurfaceView top = new SurfaceView(this);
        top.getHolder().addCallback(new ScreenCallback(SCREEN_TOP));
        setContentView(top);
        hideBars(getWindow().getDecorView());
        if (mResumed) {
            showBottom();
            nativePause(false);
        }
    }

    /* This activity again in a new process (language.txt says which). */
    private void restart() {
        android.content.Intent i = new android.content.Intent(this, MainActivity.class);
        i.putExtra("restarted", true);
        android.app.PendingIntent p = android.app.PendingIntent.getActivity(this, 0, i,
                android.app.PendingIntent.FLAG_IMMUTABLE | android.app.PendingIntent.FLAG_CANCEL_CURRENT);
        android.app.AlarmManager am = (android.app.AlarmManager) getSystemService(ALARM_SERVICE);
        am.set(android.app.AlarmManager.RTC, System.currentTimeMillis() + 300, p);
        finishAffinity();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        if (request == REQUEST_ROM && mSetup != null)
            mSetup.onRom(result == RESULT_OK && data != null ? data.getData() : null);
    }

    // Debug: `am start -n com.cer0.emerald.dev/com.cer0.emerald.MainActivity --ez capture true`
    // writes both screens to <external files>/capture_{top,bottom}.raw.
    private void handleIntent(android.content.Intent i) {
        if (mStarted && i != null && i.getBooleanExtra("capture", false)) nativeCapture();
    }

    @Override protected void onNewIntent(android.content.Intent i) {
        super.onNewIntent(i);
        handleIntent(i);
    }

    private void showBottom() {
        if (mBottom != null) return;
        DisplayManager dm = (DisplayManager) getSystemService(DISPLAY_SERVICE);
        Display[] ds = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (ds.length == 0) { Log.w(TAG, "no second display"); return; }
        Log.i(TAG, "bottom display " + ds[0].getDisplayId() + " " + ds[0].getName());
        mBottom = new BottomPresentation(this, ds[0]);
        mBottom.show();
    }

    @Override protected void onResume() {
        super.onResume();
        mResumed = true;
        if (!mStarted) return;
        showBottom();
        nativePause(false);
    }

    @Override protected void onPause() {
        mResumed = false;
        if (mStarted) nativePause(true);
        super.onPause();
    }

    @Override protected void onDestroy() {
        if (mBottom != null) { mBottom.dismiss(); mBottom = null; }
        super.onDestroy();
    }

    boolean handleKey(KeyEvent e) {
        if (!mStarted) return false;
        int a = e.getAction();
        if (a != KeyEvent.ACTION_DOWN && a != KeyEvent.ACTION_UP) return false;
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN) return false;
        if (k == KeyEvent.KEYCODE_BACK && (e.getSource() & InputDevice.SOURCE_GAMEPAD) == 0) {
            // keep the system back from closing the app; map to B
            k = KeyEvent.KEYCODE_BUTTON_B;
        }
        if (e.getRepeatCount() == 0) nativeKey(k, a == KeyEvent.ACTION_DOWN);
        return true;
    }

    boolean handleMotion(MotionEvent e) {
        if (!mStarted) return false;
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) == 0) return false;
        if (e.getAction() != MotionEvent.ACTION_MOVE) return false;
        float x = e.getAxisValue(MotionEvent.AXIS_X), y = e.getAxisValue(MotionEvent.AXIS_Y);
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (Math.abs(hx) > Math.abs(x)) x = hx;
        if (Math.abs(hy) > Math.abs(y)) y = hy;
        nativeAxis(x, y);
        return true;
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        return handleKey(e) || super.dispatchKeyEvent(e);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        return handleMotion(e) || super.dispatchGenericMotionEvent(e);
    }
}
