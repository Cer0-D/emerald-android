package com.cer0.emerald;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The cartridge languages this APK carries, and where each one lives.
 *
 * The APK has one game library per language (lib/<abi>/libemerald_<code>.so,
 * loaded by MainActivity.nativeLoadGame) and its engine files and recipe
 * (assets/lang/<code>/engine, assets/lang/<code>/builder: recipe.json,
 * literals.bin, abi.txt, rom.txt - android_port/tools/build.sh). On the
 * device every language has a root of its own, <files>/<code>/: its engine
 * files (romfs:/) and its sdmc:/ (data pack, save, settings, follower files),
 * so one language's save is never another's. <files>/language.txt names the
 * one played last.
 */
final class Languages {
    static final String TAG = "Emerald";
    static final String ACTIVE = "language.txt";

    /* A cartridge: its language code, the game code at 0xAC, its name. */
    static final class Cartridge {
        final String code, gameCode, name;
        Cartridge(String code, String gameCode, String name) {
            this.code = code;
            this.gameCode = gameCode;
            this.name = name;
        }
    }

    /* Every Emerald cartridge, carried or not: the name of one this APK
     * cannot play yet, when a player picks it. */
    static final Cartridge[] CARTRIDGES = {
        new Cartridge("en", "BPEE", "Pokémon Emerald (USA, Europe)"),
        new Cartridge("fr", "BPEF", "Pokémon Émeraude (France)"),
        new Cartridge("it", "BPEI", "Pokémon Smeraldo (Italia)"),
        new Cartridge("de", "BPED", "Pokémon Smaragd (Deutschland)"),
        new Cartridge("es", "BPES", "Pokémon Esmeralda (España)"),
        new Cartridge("ja", "BPEJ", "ポケモンエメラルド (日本)"),
    };

    /* What assets/lang/<code>/builder/rom.txt says: the cartridge's name and
     * the SHA-1 of the clean ROM its recipe rebuilds the data from. */
    static final class Rom {
        final String code, label, sha1;
        Rom(String code, String label, String sha1) {
            this.code = code;
            this.label = label;
            this.sha1 = sha1;
        }
    }

    private Languages() {}

    static boolean isDev(Context c) {
        return (c.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    /* The languages the APK carries, in assets/lang order. */
    static List<String> carried(Context c) {
        try {
            String[] names = c.getAssets().list("lang");
            if (names != null) {
                List<String> out = new ArrayList<>(Arrays.asList(names));
                out.sort(null);
                return out;
            }
        } catch (IOException e) {
            Log.e(TAG, "languages", e);
        }
        return new ArrayList<>();
    }

    static boolean carries(Context c, String code) {
        return code != null && carried(c).contains(code);
    }

    static String asset(String code, String name) { return "lang/" + code + "/" + name; }

    static Rom rom(Context c, String code) {
        try {
            String[] lines = Setup.readAsset(c.getAssets(), asset(code, "builder/rom.txt")).split("\\R");
            if (lines.length >= 3)
                return new Rom(lines[0].trim(), lines[1].trim(), lines[2].trim().toLowerCase());
        } catch (IOException e) {
            Log.e(TAG, "rom.txt of " + code, e);
        }
        return null;
    }

    static File root(Context c, String code) { return new File(MainActivity.filesDir(c), code); }

    static File packFile(Context c, String code) { return new File(root(c, code), Setup.PACK); }

    static long recipeAbi(Context c, String code) {
        try {
            return Long.parseLong(Setup.readAsset(c.getAssets(), asset(code, "builder/abi.txt")).trim(), 16);
        } catch (Exception e) {
            return -2;
        }
    }

    /* The language's pack is there and was built for this APK's library. */
    static boolean packReady(Context c, String code) {
        return PackBuilder.packAbi(packFile(c, code)) == recipeAbi(c, code);
    }

    static String active(Context c) {
        String code = readText(new File(MainActivity.filesDir(c), ACTIVE));
        return code == null ? null : code.trim();
    }

    static void setActive(Context c, String code) {
        writeText(new File(MainActivity.filesDir(c), ACTIVE), code);
    }

    /*
     * Once, after an update from a one-language app (0.5.0.1): its files sat
     * at the top of <files> - engine/ and sdmc/ (save, pack, settings) - and
     * were English. They move, renamed and not copied, into <files>/en/.
     * A development install's old romfs/ (pushed data) is left alone.
     */
    static void migrate(Context c) {
        File files = MainActivity.filesDir(c);
        File en = new File(files, "en");
        File[] old = {new File(files, "sdmc"), new File(files, Setup.ENGINE_DIR)};
        boolean any = false;
        for (File f : old) any |= f.exists();
        if (!any || en.exists()) return;
        if (!en.mkdirs()) {
            Log.e(TAG, "migration: cannot make " + en);
            return;
        }
        for (File f : old)
            if (f.exists() && !f.renameTo(new File(en, f.getName())))
                Log.e(TAG, "migration: cannot move " + f);
        if (active(c) == null) setActive(c, "en");
        Log.i(TAG, "migration: the one-language install's files moved into " + en);
    }

    /* The carried language whose clean ROM has this SHA-1, or null. */
    static String bySha1(Context c, String sha1) {
        for (String code : carried(c)) {
            Rom r = rom(c, code);
            if (r != null && r.sha1.equals(sha1)) return code;
        }
        return null;
    }

    /* The cartridge a ROM says it is (its game code), or null. */
    static Cartridge byHeader(byte[] rom) {
        if (rom.length < 0xB0) return null;
        String gameCode = new String(rom, 0xAC, 4, StandardCharsets.ISO_8859_1);
        for (Cartridge k : CARTRIDGES)
            if (k.gameCode.equals(gameCode)) return k;
        return null;
    }

    static String readText(File f) {
        try (InputStream in = new FileInputStream(f)) {
            return new String(Setup.readAll(in), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    static void writeText(File f, String text) {
        File tmp = new File(f.getPath() + ".tmp");
        f.getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.e(TAG, "cannot write " + f, e);
            return;
        }
        if (!tmp.renameTo(f)) Log.e(TAG, "cannot write " + f);
    }
}
