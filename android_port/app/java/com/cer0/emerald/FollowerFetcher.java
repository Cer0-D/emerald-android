package com.cer0.emerald;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.TreeMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * The follower Pokémon's sprites and texts (src/follower_mon.c), as Emerald
 * 3DS's builder gets them (builder/emerald3ds_builder/followers.py): HGSS's
 * files from the commits assets/followers/sources.json pins (made by
 * build_apk.sh from followers_sources.py), each checked against its size and
 * SHA-256, converted (FollowerFiles) and written to
 * sdmc/3ds/emerald3ds/followers/, where the game data layer looks for them
 * when its own data has none (3ds_data.c). None of it ships with the APK.
 *
 * In the background at every start until one run got everything; the game
 * plays without them (no follower until its sprite is there). The ROM plays
 * no part. A file is renamed into place whole, so the game never reads half
 * of one; .pin names what is being fetched, so an interrupted run's files are
 * kept, and .done what was fetched whole.
 */
final class FollowerFetcher {
    static final String TAG = "Emerald";
    static final String DIR = "sdmc/3ds/emerald3ds/followers";
    private static final String[] HOSTS = {
        "https://raw.githubusercontent.com/%1$s/%2$s/%3$s",
        "https://cdn.jsdelivr.net/gh/%1$s@%2$s/%3$s",
    };
    private static boolean sStarted;

    private FollowerFetcher() {}

    static synchronized void start(Context context, String language) {
        if (sStarted) return;
        sStarted = true;
        Context app = context.getApplicationContext();
        Thread t = new Thread(() -> {
            try {
                run(app, language);
            } catch (Throwable e) {
                Log.w(TAG, "followers: " + e);
            }
        }, "followers");
        t.setDaemon(true);
        t.setPriority(Thread.MIN_PRIORITY);
        t.start();
    }

    private static void run(Context c, String language) throws Exception {
        JSONObject sources = new JSONObject(Setup.readAsset(c.getAssets(), "followers/sources.json"));
        JSONObject repos = sources.getJSONObject("sources");
        TreeMap<String, String> sorted = new TreeMap<>();
        for (Iterator<String> it = repos.keys(); it.hasNext(); ) {
            String name = it.next();
            JSONObject s = repos.getJSONObject(name);
            sorted.put(name, name + " " + s.getString("repo") + " " + s.getString("commit") + "\n");
        }
        StringBuilder sourcesTxt = new StringBuilder();
        for (String line : sorted.values()) sourcesTxt.append(line);
        String stamp = sourcesTxt + "text " + language + "\n";

        File dir = new File(MainActivity.filesDir(c), DIR);
        File done = new File(dir, ".done");
        if (stamp.equals(readText(done))) return;
        File pin = new File(dir, ".pin");
        boolean resume = stamp.equals(readText(pin));
        new File(dir, "mon").mkdirs();
        new File(dir, "text").mkdirs();
        write(pin, stamp.getBytes(StandardCharsets.UTF_8));

        JSONArray files = sources.getJSONArray("files");
        List<JSONObject> sprites = new ArrayList<>();
        JSONObject params = null, text = null;
        for (int i = 0; i < files.length(); i++) {
            JSONObject f = files.getJSONObject(i);
            if (f.has("key")) sprites.add(f);
            else if (f.getString("path").endsWith("tp_param.csv")) params = f;
            else if (language.equals(f.optString("language"))) text = f;
        }
        long start = System.currentTimeMillis();
        AtomicInteger missing = new AtomicInteger(), fetched = new AtomicInteger();

        byte[] csv = params != null ? fetch(repos, params) : null;
        int[] flags = csv != null ? FollowerFiles.paramFlags(new String(csv, StandardCharsets.UTF_8)) : new int[0];
        if (csv == null) missing.incrementAndGet();

        ExecutorService pool = Executors.newFixedThreadPool(6);
        for (JSONObject f : sprites) {
            File out = new File(dir, "mon/" + f.getString("key") + ".bin");
            if (resume && out.isFile()) continue;
            pool.execute(() -> {
                try {
                    byte[] raw = fetch(repos, f);
                    if (raw == null) {
                        missing.incrementAndGet();
                        return;
                    }
                    int index = f.getInt("follower_index");
                    write(out, FollowerFiles.convert(raw, index < flags.length ? flags[index] : 0));
                    fetched.incrementAndGet();
                } catch (Exception e) {
                    Log.w(TAG, "followers: " + f.optString("key") + ": " + e);
                    missing.incrementAndGet();
                }
            });
        }
        pool.shutdown();
        pool.awaitTermination(1, TimeUnit.HOURS);

        File textOut = new File(dir, "text/" + language + ".bin");
        if (text == null) {
            missing.incrementAndGet();
        } else if (!(resume && textOut.isFile())) {
            byte[] raw = fetch(repos, text);
            try {
                if (raw == null) throw new IOException("not fetched");
                write(textOut, FollowerFiles.convertTexts(new String(raw, StandardCharsets.UTF_8)));
                fetched.incrementAndGet();
            } catch (Exception e) {
                Log.w(TAG, "followers: text " + language + ": " + e);
                missing.incrementAndGet();
            }
        }
        write(new File(dir, "sources.txt"), sourcesTxt.toString().getBytes(StandardCharsets.US_ASCII));
        if (missing.get() == 0) write(done, stamp.getBytes(StandardCharsets.UTF_8));
        Log.i(TAG, "followers: " + fetched.get() + " files fetched, " + missing.get() + " missing, "
                + (System.currentTimeMillis() - start) + " ms");
    }

    /** One listed file from the first host that has it whole, or null. */
    private static byte[] fetch(JSONObject repos, JSONObject entry) throws Exception {
        JSONObject src = repos.getJSONObject(entry.getString("source"));
        for (String host : HOSTS) {
            String url = String.format(host, src.getString("repo"), src.getString("commit"), entry.getString("path"));
            try {
                byte[] data = get(url);
                if (data.length == entry.getInt("size") && sha256(data).equals(entry.getString("sha256")))
                    return data;
            } catch (IOException e) {
                // the next host
            }
        }
        return null;
    }

    private static byte[] get(String url) throws IOException {
        HttpURLConnection http = (HttpURLConnection) new URL(url).openConnection();
        http.setConnectTimeout(15000);
        http.setReadTimeout(30000);
        try {
            if (http.getResponseCode() != 200) throw new IOException("HTTP " + http.getResponseCode());
            return Setup.readAll(http.getInputStream());
        } finally {
            http.disconnect();
        }
    }

    private static String sha256(byte[] data) throws Exception {
        StringBuilder s = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) s.append(String.format("%02x", b & 0xFF));
        return s.toString();
    }

    private static String readText(File f) {
        try (InputStream in = new FileInputStream(f)) {
            return new String(Setup.readAll(in), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    /** Through a temporary file, renamed into place whole. */
    private static void write(File f, byte[] data) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(data);
            out.getFD().sync();
        }
        if (!tmp.renameTo(f)) throw new IOException("could not rename " + tmp);
    }
}
