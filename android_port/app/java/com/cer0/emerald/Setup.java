package com.cer0.emerald;

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
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The game data pack (emerald3ds.pak) of a language, built here from the
 * player's own ROM, which is read and never copied or sent. The screen asks
 * for a ROM; its SHA-1 says which of the carried languages it is (Languages),
 * and that language's recipe builds the pack into that language's own root
 * (PackBuilder); then the game starts in that language.
 *
 * A development install (debuggable) takes its ROMs from <files>/roms/,
 * pushed over adb by android_port/tools/dev_rom.sh, and keeps them: it builds
 * a language's pack again by itself when a new build changes its recipe.
 */
final class Setup {
    static final String TAG = "Emerald";
    /* A language root's engine files (romfs:/ for the game). */
    static final String ENGINE_DIR = "engine";
    static final String PACK = "sdmc/3ds/emerald3ds/emerald3ds.pak";
    static final String DEV_ROMS = "roms";

    private final MainActivity mActivity;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private TextView mMessage, mStatus;
    private Button mButton;
    private ProgressBar mBar;
    private boolean mBusy;
    private Texts mTexts;

    Setup(MainActivity activity) { mActivity = activity; }

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

    /* The setup screen's own words, in the device's language (no cartridge
     * is known yet when it shows). */
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

        static Texts of(String locale, String roms) {
            switch (locale) {
                case "fr":
                    return new Texts("fr", "Émeraude pour Android",
                            "Pour jouer, choisissez votre propre ROM de Pokémon Émeraude :\n" + roms + "\n\n"
                                    + "Les données du jeu sont construites à partir d'elle sur cet appareil ; "
                                    + "la ROM n'est que lue, jamais copiée ni envoyée.",
                            "Choisir la ROM", "Lecture de la ROM", "Lecture de la recette",
                            "Les données du jeu n'ont pas pu être construites : ");
                case "de":
                    return new Texts("de", "Smaragd für Android",
                            "Um zu spielen, wähle deine eigene ROM von Pokémon Smaragd:\n" + roms + "\n\n"
                                    + "Die Spieldaten werden daraus auf diesem Gerät erzeugt; "
                                    + "die ROM wird nur gelesen, nie kopiert oder gesendet.",
                            "ROM wählen", "ROM wird gelesen", "Rezept wird gelesen",
                            "Die Spieldaten konnten nicht erzeugt werden: ");
                case "es":
                    return new Texts("es", "Esmeralda para Android",
                            "Para jugar, elige tu propia ROM de Pokémon Esmeralda:\n" + roms + "\n\n"
                                    + "Los datos del juego se crean a partir de ella en este dispositivo; "
                                    + "la ROM solo se lee, nunca se copia ni se envía.",
                            "Elegir ROM", "Leyendo la ROM", "Leyendo la receta",
                            "No se pudieron crear los datos del juego: ");
                case "it":
                    return new Texts("it", "Smeraldo per Android",
                            "Per giocare, scegli la tua ROM di Pokémon Smeraldo:\n" + roms + "\n\n"
                                    + "I dati del gioco vengono costruiti da essa su questo dispositivo; "
                                    + "la ROM viene solo letta, mai copiata o inviata.",
                            "Scegli la ROM", "Lettura della ROM", "Lettura della ricetta",
                            "Impossibile costruire i dati del gioco: ");
                default:
                    return new Texts("en", "Emerald for Android",
                            "To play, choose your own Pokémon Emerald ROM:\n" + roms + "\n\n"
                                    + "The game's data is built from it on this device; the ROM is only read, "
                                    + "never copied or sent anywhere.",
                            "Choose ROM", "Reading the ROM", "Reading the recipe",
                            "The game data could not be built: ");
            }
        }

        /* A supported cartridge's game code, but not its clean dump. */
        String notRom(String label, String expected, String actual) {
            switch (locale) {
                case "fr":
                    return "Ce n'est pas la ROM prise en charge.\n\nIl faut " + label + " (SHA-1 "
                            + expected + ") ; ce fichier a le SHA-1 " + actual + ".";
                case "de":
                    return "Dies ist nicht die unterstützte ROM.\n\nBenötigt wird " + label
                            + " (SHA-1 " + expected + "); diese Datei hat den SHA-1 " + actual + ".";
                case "es":
                    return "Esta no es la ROM compatible.\n\nSe necesita " + label + " (SHA-1 " + expected
                            + "); este archivo tiene el SHA-1 " + actual + ".";
                case "it":
                    return "Questa non è la ROM supportata.\n\nServe " + label + " (SHA-1 " + expected
                            + "); questo file ha lo SHA-1 " + actual + ".";
                default:
                    return "This is not the supported ROM.\n\n" + label + " is needed (SHA-1 " + expected
                            + "); this file's SHA-1 is " + actual + ".";
            }
        }

        /* An Emerald cartridge whose language this APK does not carry. */
        String unsupported(String name, String roms) {
            switch (locale) {
                case "fr":
                    return name + " n'est pas encore prise en charge.\n\nROM prises en charge :\n" + roms;
                case "de":
                    return name + " wird noch nicht unterstützt.\n\nUnterstützte ROMs:\n" + roms;
                case "es":
                    return name + " aún no es compatible.\n\nROM compatibles:\n" + roms;
                case "it":
                    return name + " non è ancora supportata.\n\nROM supportate:\n" + roms;
                default:
                    return name + " is not supported yet.\n\nSupported ROMs:\n" + roms;
            }
        }

        /* Not a Pokémon Emerald ROM at all. */
        String notEmerald(String actual, String roms) {
            switch (locale) {
                case "fr":
                    return "Ce fichier n'est pas une ROM de Pokémon Émeraude prise en charge (SHA-1 " + actual
                            + ").\n\nROM prises en charge :\n" + roms;
                case "de":
                    return "Diese Datei ist keine unterstützte ROM von Pokémon Smaragd (SHA-1 " + actual
                            + ").\n\nUnterstützte ROMs:\n" + roms;
                case "es":
                    return "Este archivo no es una ROM de Pokémon Esmeralda compatible (SHA-1 " + actual
                            + ").\n\nROM compatibles:\n" + roms;
                case "it":
                    return "Questo file non è una ROM di Pokémon Smeraldo supportata (SHA-1 " + actual
                            + ").\n\nROM supportate:\n" + roms;
                default:
                    return "This file is not a supported Pokémon Emerald ROM (SHA-1 " + actual
                            + ").\n\nSupported ROMs:\n" + roms;
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
                case "es":
                    switch (message) {
                        case "Checking the ROM": return "Comprobando la ROM";
                        case "Building the 3D world": return "Construyendo el mundo 3D";
                        case "Extracting the game data": return "Extrayendo los datos del juego";
                        case "Checking the data pack": return "Comprobando el paquete de datos";
                        case "Done": return "Listo";
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

    /* The carried languages' cartridges, one a line. */
    private static String romList(Context c) {
        StringBuilder s = new StringBuilder();
        for (String code : Languages.carried(c)) {
            Languages.Rom r = Languages.rom(c, code);
            if (r != null) s.append(s.length() > 0 ? "\n" : "").append("• ").append(r.label);
        }
        return s.toString();
    }

    /*
     * A language's engine files (assets/lang/<code>/engine) into its root's
     * engine/, once per install of this APK: they describe this very library
     * (its asset index, relocation tables, ABI), so an update brings new
     * ones. Files are only written over, never removed.
     */
    static void extractEngine(Context c, String code) throws IOException {
        File root = new File(Languages.root(c, code), ENGINE_DIR);
        String stamp;
        try {
            stamp = Long.toString(c.getPackageManager().getPackageInfo(c.getPackageName(), 0).lastUpdateTime);
        } catch (Exception e) {
            stamp = "?";
        }
        File mark = new File(root, ".apk");
        if (stamp.equals(Languages.readText(mark))) return;
        copyTree(c.getAssets(), Languages.asset(code, "engine"), root);
        try (OutputStream out = new FileOutputStream(mark)) {
            out.write(stamp.getBytes(StandardCharsets.UTF_8));
        }
        Log.i(TAG, code + ": engine files extracted for " + stamp);
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
        mTexts = Texts.of(Locale.getDefault().getLanguage(), romList(mActivity));
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

    private void busy() {
        mBusy = true;
        mButton.setEnabled(false);
        mBar.setVisibility(View.VISIBLE);
        mBar.setProgress(0);
        mStatus.setText(mTexts.readingRom);
    }

    void onRom(Uri uri) {
        if (uri == null || mBusy) return;
        busy();
        new Thread(() -> {
            try {
                build(readAll(mActivity.getContentResolver().openInputStream(uri)));
            } catch (Throwable e) {
                fail(e, null);
            }
        }, "pack-builder").start();
    }

    /*
     * Development install: a ROM pushed to <files>/roms/ - the one named
     * (dev_rom.sh's `--es rom NAME`), or else the one of the language played
     * last - built and played. Without one the screen stays, saying how to
     * push one (or pick one with the button).
     */
    void devRom(String name, String activeCode) {
        if (mBusy) return;
        busy();
        new Thread(() -> {
            try {
                File dir = new File(MainActivity.filesDir(mActivity), DEV_ROMS);
                byte[] rom = null;
                if (name != null) {
                    File f = new File(dir, new File(name).getName());
                    if (f.isFile()) rom = readFile(f);
                    else Log.w(TAG, "dev: no ROM " + f);
                } else if (activeCode != null) {
                    Languages.Rom want = Languages.rom(mActivity, activeCode);
                    File[] all = dir.listFiles();
                    if (want != null && all != null)
                        for (File f : all) {
                            if (!f.isFile()) continue;
                            byte[] data = readFile(f);
                            if (PackBuilder.sha1Hex(data).equals(want.sha1)) { rom = data; break; }
                        }
                }
                if (rom == null) {
                    mMain.post(() -> {
                        mBusy = false;
                        mButton.setEnabled(true);
                        mBar.setVisibility(View.GONE);
                        mStatus.setText("Development build: push a ROM with\n"
                                + "android_port/tools/dev_rom.sh ROM.gba");
                    });
                    return;
                }
                build(rom);
            } catch (Throwable e) {
                fail(e, null);
            }
        }, "pack-builder").start();
    }

    private static byte[] readFile(File f) throws IOException {
        return readAll(new FileInputStream(f));
    }

    /* Which language the ROM is, its pack built into that language's root,
     * then the game in it. */
    private void build(byte[] rom) {
        long start = System.currentTimeMillis();
        PackBuilder builder = null;
        String roms = romList(mActivity);
        try {
            String sha = PackBuilder.sha1Hex(rom);
            String code = Languages.bySha1(mActivity, sha);
            if (code == null) {
                Languages.Cartridge k = Languages.byHeader(rom);
                Languages.Rom carried = k == null ? null : Languages.rom(mActivity, k.code);
                String text = k == null ? mTexts.notEmerald(sha, roms)
                        : carried != null ? mTexts.notRom(carried.label, carried.sha1, sha)
                        : mTexts.unsupported(k.name, roms);
                failText(text);
                return;
            }
            Log.i(TAG, "ROM is " + code + " (" + sha + ")");
            post(0.0f, mTexts.readingRecipe);
            AssetManager am = mActivity.getAssets();
            builder = new PackBuilder(readAsset(am, Languages.asset(code, "builder/recipe.json")),
                    readAll(am.open(Languages.asset(code, "builder/literals.bin"))));
            builder.build(rom, Languages.packFile(mActivity, code), this::post);
            Log.i(TAG, code + ": data pack built in " + (System.currentTimeMillis() - start) + " ms ("
                    + builder.release() + ")");
            Languages.setActive(mActivity, code);
            final String chosen = code;
            mMain.post(() -> mActivity.startGame(chosen));
        } catch (Throwable e) {
            fail(e, builder);
        }
    }

    private void fail(Throwable e, PackBuilder builder) {
        Log.e(TAG, "data pack", e);
        String text;
        if (e instanceof PackBuilder.WrongRom)
            text = mTexts.notRom("?", builder == null ? "?" : builder.romSha1(), ((PackBuilder.WrongRom) e).sha1);
        else if (e instanceof PackBuilder.BuildException)
            text = e.getMessage();
        else
            text = mTexts.failed + e;
        failText(text);
    }

    private void failText(String text) {
        mMain.post(() -> {
            mBusy = false;
            mButton.setEnabled(true);
            mBar.setVisibility(View.GONE);
            mStatus.setText(text);
        });
    }

    private void post(float fraction, String message) {
        final String shown = mTexts.step(message);
        mMain.post(() -> {
            mBar.setProgress((int) (fraction * 1000));
            mStatus.setText(shown);
        });
    }
}
