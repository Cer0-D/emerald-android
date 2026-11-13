package com.cer0.emerald;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * First boot of a release APK: the game data pack (emerald3ds.pak) is built
 * here from the player's own ROM, which is read once and never copied. Until
 * the pack is there and made for this build (its ABI the library's), this
 * screen asks for the ROM and builds it (PackBuilder); then the game starts.
 */
final class Setup {
    static final String TAG = "Emerald";
    /* Where a release's engine files go (romfs:/ for the game): beside a
     * development install's romfs/, never over it. */
    static final String ENGINE_DIR = "engine";
    static final String PACK = "sdmc/3ds/emerald3ds/emerald3ds.pak";

    private final MainActivity mActivity;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private TextView mMessage, mStatus;
    private Button mButton;
    private ProgressBar mBar;
    private boolean mBusy;
    private String mRomLabel;
    private Texts mTexts;

    Setup(MainActivity activity) { mActivity = activity; }

    static boolean isRelease(Context c) {
        try (InputStream in = c.getAssets().open("builder/abi.txt")) {
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    static long recipeAbi(Context c) {
        try {
            return Long.parseLong(readAsset(c.getAssets(), "builder/abi.txt").trim(), 16);
        } catch (Exception e) {
            return -2;
        }
    }

    static File packFile(Context c) { return new File(MainActivity.filesDir(c), PACK); }

    /* The pack is there and was built for this APK's library. */
    static boolean packReady(Context c) {
        return PackBuilder.packAbi(packFile(c)) == recipeAbi(c);
    }

    static String readAsset(AssetManager am, String path) throws IOException {
        return new String(readAll(am.open(path)), StandardCharsets.UTF_8);
    }

    static byte[] readAll(InputStream in) throws IOException {
        try (InputStream s = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(s.available(), 1 << 16));
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = s.read(buf)) > 0) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    /*
     * The cartridge this build was made for (assets/builder/rom.txt, written by
     * tools/release.sh): the language code, then the name the setup screen
     * shows. A development install has no file and keeps the English wording.
     */
    static String[] romInfo(Context c) {
        try {
            String[] lines = readAsset(c.getAssets(), "builder/rom.txt").split("\\R", 2);
            if (lines.length == 2 && !lines[0].trim().isEmpty() && !lines[1].trim().isEmpty())
                return new String[]{lines[0].trim(), lines[1].trim()};
        } catch (IOException e) {
            // no rom.txt: the development tree's English ROM
        }
        return new String[]{"en", "Pokémon Emerald (USA, Europe)"};
    }

    /* The setup screen's own words, in the build's language. */
    static final class Texts {
        final String locale;
        final String title, message, choose, readingRom, readingRecipe, failed;

        Texts(String locale, String title, String message, String choose, String readingRom,
              String readingRecipe, String failed) {
            this.locale = locale;
            this.title = title;
            this.message = message;
            this.choose = choose;
            this.readingRom = readingRom;
            this.readingRecipe = readingRecipe;
            this.failed = failed;
        }

        static Texts of(String locale, String rom) {
            switch (locale) {
                case "fr":
                    return new Texts("fr", "Émeraude pour Android",
                            "Pour jouer, choisissez votre propre ROM " + rom + ".\n"
                                    + "Les données du jeu sont construites à partir d'elle sur cet appareil ; "
                                    + "la ROM n'est que lue, jamais copiée ni envoyée.",
                            "Choisir la ROM", "Lecture de la ROM", "Lecture de la recette",
                            "Les données du jeu n'ont pas pu être construites : ");
                case "de":
                    return new Texts("de", "Smaragd für Android",
                            "Um zu spielen, wähle deine eigene ROM " + rom + ".\n"
                                    + "Die Spieldaten werden daraus auf diesem Gerät erzeugt; "
                                    + "die ROM wird nur gelesen, nie kopiert oder gesendet.",
                            "ROM wählen", "ROM wird gelesen", "Rezept wird gelesen",
                            "Die Spieldaten konnten nicht erzeugt werden: ");
                case "it":
                    return new Texts("it", "Smeraldo per Android",
                            "Per giocare, scegli la tua ROM " + rom + ".\n"
                                    + "I dati del gioco vengono costruiti da essa su questo dispositivo; "
                                    + "la ROM viene solo letta, mai copiata o inviata.",
                            "Scegli la ROM", "Lettura della ROM", "Lettura della ricetta",
                            "Impossibile costruire i dati del gioco: ");
                default:
                    return new Texts("en", "Emerald for Android",
                            "To play, choose your own " + rom + ".\n"
                                    + "The game's data is built from it on this device; the ROM is only read, "
                                    + "never copied or sent anywhere.",
                            "Choose ROM", "Reading the ROM", "Reading the recipe",
                            "The game data could not be built: ");
            }
        }

        String notRom(String label, String expected, String actual) {
            switch (locale) {
                case "fr":
                    return "Ce n'est pas la ROM prise en charge.\n\nIl faut " + label + " (SHA-1 "
                            + expected + ") ; ce fichier a le SHA-1 " + actual + ".";
                case "de":
                    return "Dies ist nicht die unterstützte ROM.\n\nBenötigt wird " + label
                            + " (SHA-1 " + expected + "); diese Datei hat den SHA-1 " + actual + ".";
                case "it":
                    return "Questa non è la ROM supportata.\n\nServe " + label + " (SHA-1 " + expected
                            + "); questo file ha lo SHA-1 " + actual + ".";
                default:
                    return "This is not the supported ROM.\n\n" + label + " is needed (SHA-1 " + expected
                            + "); this file's SHA-1 is " + actual + ".";
            }
        }

        /* PackBuilder's progress lines, in the setup screen's language. */
        String step(String message) {
            switch (locale) {
                case "fr":
                    switch (message) {
                        case "Checking the ROM": return "Vérification de la ROM";
                        case "Building the 3D world": return "Construction du monde 3D";
                        case "Extracting the game data": return "Extraction des données du jeu";
                        case "Checking the data pack": return "Vérification du pack de données";
                        case "Done": return "Terminé";
                    }
                    break;
                case "de":
                    switch (message) {
                        case "Checking the ROM": return "ROM wird geprüft";
                        case "Building the 3D world": return "3D-Welt wird aufgebaut";
                        case "Extracting the game data": return "Spieldaten werden extrahiert";
                        case "Checking the data pack": return "Datenpaket wird geprüft";
                        case "Done": return "Fertig";
                    }
                    break;
                case "it":
                    switch (message) {
                        case "Checking the ROM": return "Verifica della ROM";
                        case "Building the 3D world": return "Costruzione del mondo 3D";
                        case "Extracting the game data": return "Estrazione dei dati di gioco";
                        case "Checking the data pack": return "Verifica del pacchetto dati";
                        case "Done": return "Fatto";
                    }
                    break;
            }
            return message;
        }
    }

    /*
     * The APK's engine files (assets/engine) into files/engine, once per
     * install of this APK: they describe this very library (its asset index,
     * relocation tables, ABI), so an update brings new ones. Files are only
     * written over, never removed.
     */
    static void extractEngine(Context c) throws IOException {
        File root = new File(MainActivity.filesDir(c), ENGINE_DIR);
        String stamp;
        try {
            stamp = Long.toString(c.getPackageManager().getPackageInfo(c.getPackageName(), 0).lastUpdateTime);
        } catch (Exception e) {
            stamp = "?";
        }
        File mark = new File(root, ".apk");
        if (mark.isFile()) {
            try (InputStream in = new java.io.FileInputStream(mark)) {
                if (new String(readAll(in), StandardCharsets.UTF_8).equals(stamp)) return;
            }
        }
        copyTree(c.getAssets(), "engine", root);
        try (OutputStream out = new FileOutputStream(mark)) {
            out.write(stamp.getBytes(StandardCharsets.UTF_8));
        }
        Log.i(TAG, "engine files extracted for " + stamp);
    }

    private static void copyTree(AssetManager am, String path, File dest) throws IOException {
        String[] names = am.list(path);
        if (names == null || names.length == 0) {
            dest.getParentFile().mkdirs();
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            return;
        }
        dest.mkdirs();
        for (String name : names) copyTree(am, path + "/" + name, new File(dest, name));
    }

    // ── The screen ───────────────────────────────────────────────────────

    void show() {
        String[] rom = romInfo(mActivity);
        mRomLabel = rom[1];
        mTexts = Texts.of(rom[0], mRomLabel);
        LinearLayout root = new LinearLayout(mActivity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(16, 40, 32));
        root.setPadding(96, 48, 96, 48);

        TextView title = new TextView(mActivity);
        title.setText(mTexts.title);
        title.setTextColor(Color.rgb(120, 220, 160));
        title.setTextSize(30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        mMessage = new TextView(mActivity);
        mMessage.setText(mTexts.message);
        mMessage.setTextColor(Color.WHITE);
        mMessage.setTextSize(18);
        mMessage.setGravity(Gravity.CENTER);
        mMessage.setPadding(0, 32, 0, 32);
        root.addView(mMessage);

        mButton = new Button(mActivity);
        mButton.setText(mTexts.choose);
        mButton.setTextSize(20);
        mButton.setOnClickListener(v -> pickRom());
        root.addView(mButton, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        mBar = new ProgressBar(mActivity, null, android.R.attr.progressBarStyleHorizontal);
        mBar.setMax(1000);
        mBar.setVisibility(View.GONE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 32;
        root.addView(mBar, lp);

        mStatus = new TextView(mActivity);
        mStatus.setTextColor(Color.rgb(180, 200, 190));
        mStatus.setTextSize(16);
        mStatus.setGravity(Gravity.CENTER);
        mStatus.setPadding(0, 16, 0, 0);
        root.addView(mStatus);

        mActivity.setContentView(root);
        MainActivity.hideBars(mActivity.getWindow().getDecorView());
        mButton.requestFocus();
    }

    private void pickRom() {
        if (mBusy) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        mActivity.startActivityForResult(i, MainActivity.REQUEST_ROM);
    }

    void onRom(Uri uri) {
        if (uri == null || mBusy) return;
        mBusy = true;
        mButton.setEnabled(false);
        mBar.setVisibility(View.VISIBLE);
        mBar.setProgress(0);
        mStatus.setText(mTexts.readingRom);
        new Thread(() -> build(uri), "pack-builder").start();
    }

    private void build(Uri uri) {
        long start = System.currentTimeMillis();
        PackBuilder builder = null;
        try {
            byte[] rom = readAll(mActivity.getContentResolver().openInputStream(uri));
            post(0.0f, mTexts.readingRecipe);
            AssetManager am = mActivity.getAssets();
            builder = new PackBuilder(readAsset(am, "builder/recipe.json"),
                    readAll(am.open("builder/literals.bin")));
            builder.build(rom, packFile(mActivity), this::post);
            Log.i(TAG, "data pack built in " + (System.currentTimeMillis() - start) + " ms (" + builder.release() + ")");
            mMain.post(() -> mActivity.startGame(ENGINE_DIR));
        } catch (Throwable e) {
            Log.e(TAG, "data pack", e);
            String text;
            if (e instanceof PackBuilder.WrongRom)
                text = mTexts.notRom(mRomLabel, builder == null ? "?" : builder.romSha1(),
                        ((PackBuilder.WrongRom) e).sha1);
            else if (e instanceof PackBuilder.BuildException)
                text = e.getMessage();
            else
                text = mTexts.failed + e;
            final String shown = text;
            mMain.post(() -> {
                mBusy = false;
                mButton.setEnabled(true);
                mBar.setVisibility(View.GONE);
                mStatus.setText(shown);
            });
        }
    }

    private void post(float fraction, String message) {
        final String shown = mTexts.step(message);
        mMain.post(() -> {
            mBar.setProgress((int) (fraction * 1000));
            mStatus.setText(shown);
        });
    }
}
