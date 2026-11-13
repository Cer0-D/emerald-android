package com.cer0.emerald;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.CRC32;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * The player's ROM to emerald3ds.pak, on the device: the desktop and web
 * builders' work (builder/emerald3ds_builder: recipe.py, precompiled.py,
 * pak.py) in Java, byte for byte.
 *
 * The recipe comes with the APK, unpacked by android_port/tools/export_recipe.py:
 * recipe.json (the metadata: every data file as operations over the ROM) and
 * literals.bin (the pool of bytes that are not the game's: pointers into this
 * build's library, the port's own data, the voxel programs). Every file is
 * checked against its CRC-32, and the pack's ABI against the one the library
 * was built with, as the desktop builder checks them. The ROM is only read.
 */
final class PackBuilder {
    interface Progress { void update(float fraction, String message); }

    static class BuildException extends Exception {
        BuildException(String message) { super(message); }
    }

    /** A ROM that is not the one this build's recipe was made from. */
    static final class WrongRom extends BuildException {
        final String sha1;
        WrongRom(String sha1) { super("not the supported ROM: " + sha1); this.sha1 = sha1; }
    }

    private final JSONObject meta;
    private final byte[] literals;
    private final List<int[]> bitmaps = new ArrayList<>();
    private final Map<String, JSONObject> inputs = new HashMap<>();
    private final Map<String, byte[]> inputCache = new HashMap<>();
    private byte[] rom;

    PackBuilder(String recipeJson, byte[] literals) throws Exception {
        this.meta = new JSONObject(recipeJson);
        this.literals = literals;
        JSONArray bm = meta.optJSONArray("bitmaps");
        if (bm != null)
            for (int i = 0; i < bm.length(); i++) {
                JSONArray b = bm.getJSONArray(i);
                int[] map = new int[b.length()];
                for (int k = 0; k < map.length; k++) map[k] = b.getInt(k);
                bitmaps.add(map);
            }
        JSONArray in = meta.optJSONArray("inputs");
        if (in != null)
            for (int i = 0; i < in.length(); i++) {
                JSONObject e = in.getJSONObject(i);
                inputs.put(e.getString("path"), e);
            }
    }

    long engineAbi() { return meta.optLong("engine_abi", -1); }
    String romSha1() { return meta.optString("rom_sha1", ""); }
    String release() { return meta.optString("release", "?"); }

    static String sha1Hex(byte[] data) throws Exception {
        byte[] d = MessageDigest.getInstance("SHA-1").digest(data);
        StringBuilder s = new StringBuilder();
        for (byte b : d) s.append(String.format("%02x", b & 0xFF));
        return s.toString();
    }

    static long crc32(byte[] data) {
        CRC32 c = new CRC32();
        c.update(data);
        return c.getValue();
    }

    /** pak.path_id: FNV-1a 64 of the path's UTF-8. */
    static long pathId(String path) {
        long value = 0xcbf29ce484222325L;
        for (byte b : path.getBytes(StandardCharsets.UTF_8)) {
            value ^= (b & 0xFF);
            value *= 0x100000001b3L;
        }
        return value;
    }

    static int entryType(String path) {
        String[][] types = {{"graphics/", "1"}, {"data/", "1"}, {"generated/", "1"}, {"maps/", "2"},
                            {"scripts/", "3"}, {"sound/", "4"}, {"gamedata/", "5"}, {"voxel/", "6"}};
        for (String[] t : types) if (path.startsWith(t[0])) return Integer.parseInt(t[1]);
        return 0;
    }

    /** pak.engine_abi over (path, size, crc), sorted by path. */
    static long engineAbi(List<Object[]> items) {
        List<Object[]> sorted = new ArrayList<>(items);
        sorted.sort((a, b) -> ((String) a[0]).compareTo((String) b[0]));
        StringBuilder text = new StringBuilder();
        for (Object[] it : sorted)
            text.append((String) it[0]).append('\n').append((int) it[1]).append('\n')
                .append(String.format("%08x", (long) it[2])).append('\n');
        long v = crc32(text.toString().getBytes(StandardCharsets.UTF_8));
        return v != 0 ? v : 1;
    }

    // ── recipe.py ────────────────────────────────────────────────────────

    static final class Out {
        byte[] buf = new byte[1024];
        int len;
        void ensure(int more) {
            if (len + more > buf.length) buf = Arrays.copyOf(buf, Math.max(buf.length * 2, len + more));
        }
        void add(byte[] src, int off, int n) { ensure(n); System.arraycopy(src, off, buf, len, n); len += n; }
        void add(int b) { ensure(1); buf[len++] = (byte) b; }
        void fill(int b, int n) { ensure(n); Arrays.fill(buf, len, len + n, (byte) b); len += n; }
        byte[] bytes() { return Arrays.copyOf(buf, len); }
    }

    /** GBA BIOS LZ77 (type 0x10). */
    static void lz77(byte[] src, int offset, Out out) throws BuildException {
        if ((src[offset] & 0xFF) != 0x10) throw new BuildException("no LZ77 header at " + offset);
        int size = (src[offset + 1] & 0xFF) | (src[offset + 2] & 0xFF) << 8 | (src[offset + 3] & 0xFF) << 16;
        int start = out.len, pos = offset + 4;
        out.ensure(size);
        while (out.len - start < size) {
            int flags = src[pos++] & 0xFF;
            for (int bit = 0; bit < 8 && out.len - start < size; bit++) {
                if ((flags & (0x80 >> bit)) != 0) {
                    int a = src[pos] & 0xFF, b = src[pos + 1] & 0xFF;
                    pos += 2;
                    int length = (a >> 4) + 3, back = ((a & 0xF) << 8 | b) + 1;
                    for (int k = 0; k < length; k++) {
                        out.ensure(1);
                        out.buf[out.len] = out.buf[out.len - back];
                        out.len++;
                    }
                } else
                    out.add(src[pos++] & 0xFF);
            }
        }
        out.len = start + size;
    }

    byte[] buildEntry(JSONObject entry, boolean withSources) throws Exception {
        String path = entry.getString("path");
        JSONArray ops = entry.getJSONArray("ops");
        Out out = new Out();
        for (int i = 0; i < ops.length(); i++) {
            JSONArray op = ops.getJSONArray(i);
            String kind = op.getString(0);
            switch (kind) {
                case "C": {
                    int off = op.getInt(1), n = op.getInt(2);
                    if (off < 0 || (long) off + n > rom.length) throw new BuildException(path + ": ROM range out of bounds");
                    out.add(rom, off, n);
                    break;
                }
                case "F": out.fill(op.getInt(1), op.getInt(2)); break;
                case "L": out.add(literals, op.getInt(1), op.getInt(2)); break;
                case "Z": lz77(rom, op.getInt(1), out); break;
                case "R": {
                    int off = op.getInt(1), srcStride = op.getInt(2), dstStride = op.getInt(3), count = op.getInt(4);
                    int[] bitmap = bitmaps.get(op.getInt(5));
                    for (int k = 0; k < count; k++) {
                        byte[] rec = new byte[dstStride];
                        int base = off + k * srcStride;
                        for (int b = 0; b < bitmap.length; b++) {
                            int s = bitmap[b], bit;
                            if (s == -1) bit = 0;
                            else if (s == -2) bit = 1;
                            else bit = (rom[base + (s >> 3)] >> (s & 7)) & 1;
                            if (bit != 0) rec[b >> 3] |= (byte) (1 << (b & 7));
                        }
                        out.add(rec, 0, dstStride);
                    }
                    break;
                }
                case "V": {
                    if (!withSources) throw new BuildException(path + ": a program with nothing to read");
                    int off = op.getInt(1), n = op.getInt(2);
                    if (off < 0 || n < 0 || (long) off + n > literals.length)
                        throw new BuildException(path + ": damaged program reference");
                    new Program(Arrays.copyOfRange(literals, off, off + n), entry.getInt("size") - out.len).run(out);
                    break;
                }
                default: throw new BuildException(path + ": unknown operation " + kind);
            }
        }
        byte[] data = out.bytes();
        JSONArray patches = entry.optJSONArray("patches");
        if (patches != null)
            for (int i = 0; i < patches.length(); i++) {
                JSONArray p = patches.getJSONArray(i);
                System.arraycopy(literals, p.getInt(1), data, p.getInt(0), p.getInt(2));
            }
        if (data.length != entry.getInt("size") || crc32(data) != entry.getLong("crc"))
            throw new BuildException(String.format("%s does not match the expected result (size %d/%d, CRC %08x/%08x)",
                    path, data.length, entry.getInt("size"), crc32(data), entry.getLong("crc")));
        return data;
    }

    /** A voxel input (tiles.4bpp, palettes/NN.gbapal), rebuilt from the ROM once. */
    byte[] input(String path) throws Exception {
        byte[] got = inputCache.get(path);
        if (got == null) {
            JSONObject e = inputs.get(path);
            if (e == null) throw new BuildException("missing input " + path);
            got = buildEntry(e, false);
            inputCache.put(path, got);
        }
        return got;
    }

    // ── precompiled.py ───────────────────────────────────────────────────

    static int texel(int colour, int alpha) {
        return (colour & 31) << 11 | (colour >> 5 & 31) << 6 | (colour >> 10 & 31) << 1 | alpha;
    }

    private static byte[] sGlass;

    static int glass(int value) {
        if (sGlass == null) {
            byte[] g = new byte[1 << 15];
            for (int v = 0; v < g.length; v++) {
                int t = v << 1;
                int r = (t >> 11 & 31) * 255 / 31, gr = (t >> 6 & 31) * 255 / 31, b = (t >> 1 & 31) * 255 / 31;
                g[v] = (byte) Math.min(255, 128 + (r + gr + b) / 6);
            }
            sGlass = g;
        }
        return sGlass[(value & 0xFFFF) >> 1] & 0xFF;
    }

    /** precompiled.pica_pairs: for each output pair, the raster pair it is. */
    static int[] picaPairs(int w, int h) throws BuildException {
        if (w <= 0 || h <= 0 || w % 8 != 0 || h % 8 != 0)
            throw new BuildException("a PICA texture is whole 8x8 tiles, not " + w + "x" + h);
        int half = w / 2;
        int[] tile = new int[32];
        for (int m = 0; m < 32; m++) {
            int dx = (m >> 1 & 1) | (m >> 3 & 1) << 1;
            int dy = (m & 1) | (m >> 2 & 1) << 1 | (m >> 4 & 1) << 2;
            tile[m] = dy * half + dx;
        }
        int[] perm = new int[w * h / 2];
        int k = 0;
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < half; tx += 4) {
                int base = ty * half + tx;
                for (int t : tile) perm[k++] = base + t;
            }
        return perm;
    }

    final class Program {
        final byte[] p;
        int pos;
        final int limit;
        final List<String> paths = new ArrayList<>();
        final List<int[]> srcs = new ArrayList<>();
        final Map<Long, byte[]> rows = new HashMap<>();
        final Map<String, int[]> palettes = new HashMap<>();

        Program(byte[] program, int limit) { this.p = program; this.limit = limit; }

        int u8() throws BuildException {
            if (pos >= p.length) throw new BuildException("program truncated");
            return p[pos++] & 0xFF;
        }

        int varint() throws BuildException {
            long value = 0;
            int shift = 0;
            while (true) {
                if (pos >= p.length || shift > 56) throw new BuildException("damaged number in the program");
                int b = p[pos++] & 0xFF;
                value |= (long) (b & 0x7F) << shift;
                if (b < 0x80) {
                    if (value > Integer.MAX_VALUE) throw new BuildException("number too large in the program");
                    return (int) value;
                }
                shift += 7;
            }
        }

        int[] palette(String path) throws Exception {
            int[] pal = palettes.get(path);
            if (pal == null) {
                byte[] raw = input(path);
                if (raw.length < 32) throw new BuildException("palette " + path + " is " + raw.length + " bytes");
                pal = new int[16];
                for (int i = 0; i < 16; i++) pal[i] = (raw[2 * i] & 0xFF) | (raw[2 * i + 1] & 0xFF) << 8;
                palettes.put(path, pal);
            }
            return pal;
        }

        byte[] tileRow(int s, int row, boolean left, int alpha) throws Exception {
            long key = ((long) s << 16) | (row << 2) | (left ? 2 : 0) | alpha;
            byte[] got = rows.get(key);
            if (got == null) {
                int[] src = srcs.get(s);
                byte[] data = input(paths.get(src[0]));
                int[] pal = palette(paths.get(src[2]));
                int base = src[1] * 32 + row * 4;
                if (base + 4 > data.length) throw new BuildException("tile " + src[1] + " past the end of " + paths.get(src[0]));
                int[] idx = new int[8];
                for (int i = 0; i < 4; i++) {
                    int b = data[base + i] & 0xFF;
                    idx[2 * i] = b & 15;
                    idx[2 * i + 1] = b >> 4;
                }
                got = new byte[16];
                for (int i = 0; i < 8; i++) {
                    int t = texel(pal[idx[left ? 7 - i : i]], alpha);
                    got[2 * i] = (byte) t;
                    got[2 * i + 1] = (byte) (t >> 8);
                }
                rows.put(key, got);
            }
            return got;
        }

        void room(Out out, long n, int start) throws BuildException {
            if (n < 0 || (out.len - start) + n > limit) throw new BuildException("the program writes past the file's size");
        }

        void run(Out out) throws Exception {
            int start = out.len, lastAt = -1, lastTotal = 0;
            while (pos < p.length) {
                int op = u8();
                switch (op) {
                    case 1: { int n = varint(); room(out, n, start);
                        if (pos + n > p.length) throw new BuildException("program truncated");
                        out.add(p, pos, n); pos += n; break; }
                    case 2: { int n = varint(); room(out, n, start); out.fill(0, n); break; }
                    case 3: { int src = varint(), n = varint(); room(out, n, start);
                        if (src + n > out.len - start) throw new BuildException("copy past what is written");
                        out.ensure(n); System.arraycopy(out.buf, start + src, out.buf, out.len, n); out.len += n; break; }
                    case 4: { int n = varint();
                        for (int i = 0; i < n; i++) { int len = varint();
                            if (pos + len > p.length) throw new BuildException("program truncated");
                            paths.add(new String(p, pos, len, StandardCharsets.UTF_8)); pos += len; }
                        break; }
                    case 5: { int n = varint();
                        for (int i = 0; i < n; i++) srcs.add(new int[]{varint(), varint(), varint()});
                        break; }
                    case 6: {
                        int w = varint(), h = varint(), order = u8();
                        int total = w * h;
                        room(out, 2L * total, start);
                        byte[] block = new byte[2 * total];
                        int bl = 0;
                        while (bl < 2 * total) {
                            int k = u8(), kind = k & 3;
                            if (kind == 0) {
                                int n = varint();
                                if (bl + 2 * n > 2 * total) break;
                                bl += 2 * n;
                            } else if (kind == 1) {
                                int n = varint();
                                if (bl + 2 * n > 2 * total) break;
                                if (pos + 2 * n > p.length) throw new BuildException("program truncated");
                                System.arraycopy(p, pos, block, bl, 2 * n); pos += 2 * n; bl += 2 * n;
                            } else if (kind == 2) {
                                int n = (k >> 2 & 7) + 1, s = varint(), b = u8(), col = b >> 3 & 7;
                                if (col + n > 8) throw new BuildException("tile run past its row");
                                if (bl + 2 * n > 2 * total) throw new BuildException("texel runs overrun their block");
                                byte[] row = tileRow(s, b & 7, (b & 0x40) != 0, (b & 0x80) != 0 ? 0 : 1);
                                System.arraycopy(row, 2 * col, block, bl, 2 * n); bl += 2 * n;
                            } else {
                                int palId = varint(), b = u8(), n = varint();
                                if (bl + 2 * n > 2 * total) break;
                                int v = texel(palette(paths.get(palId))[b & 15], (b & 0x80) != 0 ? 0 : 1);
                                for (int i = 0; i < n; i++) { block[bl++] = (byte) v; block[bl++] = (byte) (v >> 8); }
                            }
                        }
                        if (bl != 2 * total) throw new BuildException("texel runs overrun their block");
                        if (order == 1) {
                            int[] perm = picaPairs(w, h);
                            byte[] re = new byte[block.length];
                            for (int j = 0; j < perm.length; j++) System.arraycopy(block, 4 * perm[j], re, 4 * j, 4);
                            block = re;
                        } else if (order != 0) throw new BuildException("unknown texel order " + order);
                        lastAt = out.len;
                        lastTotal = total;
                        out.add(block, 0, block.length);
                        break;
                    }
                    case 9: {
                        if (lastAt < 0) throw new BuildException("glass with no texels before it");
                        room(out, lastTotal, start);
                        int at = lastAt, gl = 0;
                        byte[] glow = new byte[lastTotal];
                        while (gl < lastTotal) {
                            int k = u8(), n = varint();
                            if (gl + n > lastTotal) throw new BuildException("glass runs overrun their block");
                            if (k == 0) gl += n;
                            else if (k == 1) {
                                if (pos + n > p.length) throw new BuildException("program truncated");
                                System.arraycopy(p, pos, glow, gl, n); pos += n; gl += n;
                            } else if (k == 2) {
                                for (int j = 0; j < n; j++) {
                                    int i = at + 2 * (gl + j);
                                    glow[gl + j] = (byte) glass((out.buf[i] & 0xFF) | (out.buf[i + 1] & 0xFF) << 8);
                                }
                                gl += n;
                            } else throw new BuildException("unknown glass run " + k);
                        }
                        out.add(glow, 0, glow.length);
                        break;
                    }
                    case 7: {
                        int palId = varint(), index = u8();
                        room(out, 2, start);
                        int v = texel(palette(paths.get(palId))[index & 15], 1);
                        out.add(v & 0xFF);
                        out.add(v >> 8 & 0xFF);
                        break;
                    }
                    case 8: {
                        int tilesId = varint(), tile = varint(), pixel = u8(), high = u8();
                        room(out, 1, start);
                        byte[] data = input(paths.get(tilesId));
                        if (tile * 32 + 32 > data.length || pixel > 63)
                            throw new BuildException("pixel " + pixel + " of tile " + tile + " past " + paths.get(tilesId));
                        int b = data[tile * 32 + pixel / 2] & 0xFF;
                        out.add(high | ((pixel & 1) != 0 ? b >> 4 : b & 15));
                        break;
                    }
                    default: throw new BuildException("unknown operation " + op + " at " + (pos - 1));
                }
            }
        }
    }

    // ── build.py + pak.py ────────────────────────────────────────────────

    /**
     * Writes the pack to out (through out.tmp, renamed when whole and
     * checked). Every file the recipe lists, the precompiled voxel files
     * included, in the pack's id order: each is made, checked, written at its
     * place and dropped, so the build never holds more than one file and the
     * voxel inputs.
     */
    void build(byte[] romData, File out, Progress progress) throws Exception {
        rom = romData;
        progress.update(0.0f, "Checking the ROM");
        String sha = sha1Hex(rom);
        if (!sha.equals(romSha1()))
            throw new WrongRom(sha);

        List<JSONObject> all = new ArrayList<>();
        JSONArray entries = meta.getJSONArray("entries");
        for (int i = 0; i < entries.length(); i++) all.add(entries.getJSONObject(i));
        JSONArray gen = meta.optJSONArray("generated");
        if (gen != null)
            for (int i = 0; i < gen.length(); i++) {
                JSONObject e = gen.getJSONObject(i);
                if (!e.has("ops"))
                    throw new BuildException(e.getString("path") + " has no program in this release's recipe.");
                all.add(e);
            }

        // The ABI from the recipe's sizes and CRCs: the files are checked
        // against both as they are made.
        List<Object[]> items = new ArrayList<>();
        for (JSONObject e : all) items.add(new Object[]{e.getString("path"), e.getInt("size"), e.getLong("crc")});
        long abi = engineAbi(items);
        if (abi != engineAbi())
            throw new BuildException(String.format("The recipe does not match its library (ABI %08x, expected %08x).", abi, engineAbi()));

        Map<JSONObject, Long> ids = new HashMap<>();
        for (JSONObject e : all) ids.put(e, pathId(e.getString("path")));
        all.sort((a, b) -> Long.compareUnsigned(ids.get(a), ids.get(b)));
        int count = all.size();
        long indexOffset = 64, dataOffset = (indexOffset + count * 40L + 31) / 32 * 32;
        byte[] index = new byte[count * 40];
        long cursor = dataOffset;
        long[] offsets = new long[count];
        for (int i = 0; i < count; i++) {
            JSONObject e = all.get(i);
            String path = e.getString("path");
            int size = e.getInt("size");
            offsets[i] = cursor;
            putLong(index, i * 40, pathId(path));
            putInt(index, i * 40 + 8, entryType(path));
            putInt(index, i * 40 + 12, 0);
            putLong(index, i * 40 + 16, cursor);
            putInt(index, i * 40 + 24, size);
            putInt(index, i * 40 + 28, size);
            putInt(index, i * 40 + 32, (int) e.getLong("crc"));
            putInt(index, i * 40 + 36, 0);
            cursor = (cursor + size + 31) / 32 * 32;
        }
        for (int i = 1; i < count; i++)
            if (Long.compareUnsigned(getLong(index, (i - 1) * 40), getLong(index, i * 40)) >= 0)
                throw new BuildException("Two data files share an id: " + all.get(i).getString("path"));

        byte[] head = new byte[64];
        System.arraycopy("EM3DPAK\0".getBytes(StandardCharsets.US_ASCII), 0, head, 0, 8);
        putInt(head, 8, 1);
        putInt(head, 12, (int) abi);
        byte[] shaBytes = MessageDigest.getInstance("SHA-1").digest(rom);
        System.arraycopy(shaBytes, 0, head, 16, 20);
        putInt(head, 36, count);
        putLong(head, 40, indexOffset);
        putLong(head, 48, dataOffset);
        putInt(head, 56, (int) crc32(index));
        CRC32 hc = new CRC32();
        hc.update(head, 0, 60);
        putInt(head, 60, (int) hc.getValue());

        File tmp = new File(out.getPath() + ".tmp");
        out.getParentFile().mkdirs();
        try (RandomAccessFile f = new RandomAccessFile(tmp, "rw")) {
            f.setLength(cursor);
            f.seek(0);
            f.write(head);
            f.write(index);
            for (int i = 0; i < count; i++) {
                JSONObject e = all.get(i);
                byte[] data = buildEntry(e, true);
                f.seek(offsets[i]);
                f.write(data);
                if (i % 64 == 0 || i == count - 1)
                    progress.update(0.02f + 0.93f * (i + 1) / count,
                            e.getString("path").startsWith("voxel/") ? "Building the 3D world" : "Extracting the game data");
            }
            f.getFD().sync();
        } catch (Exception ex) {
            tmp.delete();
            throw ex;
        }
        progress.update(0.96f, "Checking the data pack");
        verify(tmp, all, offsets);
        if (!tmp.renameTo(out))
            throw new IOException("The data pack could not be moved into place.");
        inputCache.clear();
        progress.update(1.0f, "Done");
    }

    /** pak.PakReader.verify: every payload read back against its CRC. */
    private static void verify(File file, List<JSONObject> all, long[] offsets) throws Exception {
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            for (int i = 0; i < all.size(); i++) {
                byte[] data = new byte[all.get(i).getInt("size")];
                f.seek(offsets[i]);
                f.readFully(data);
                if (crc32(data) != all.get(i).getLong("crc"))
                    throw new BuildException("The data pack did not read back correctly (" + all.get(i).getString("path") + ").");
            }
        }
    }

    /** The ABI of a pack already on the device, or -1 when it is not one. */
    static long packAbi(File file) {
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            byte[] head = new byte[64];
            f.readFully(head);
            if (!new String(head, 0, 8, StandardCharsets.US_ASCII).equals("EM3DPAK\0")) return -1;
            CRC32 c = new CRC32();
            c.update(head, 0, 60);
            if ((int) c.getValue() != getInt(head, 60)) return -1;
            return getInt(head, 12) & 0xFFFFFFFFL;
        } catch (IOException e) {
            return -1;
        }
    }

    static void putInt(byte[] b, int at, int v) {
        b[at] = (byte) v; b[at + 1] = (byte) (v >> 8); b[at + 2] = (byte) (v >> 16); b[at + 3] = (byte) (v >> 24);
    }

    static void putLong(byte[] b, int at, long v) {
        putInt(b, at, (int) v);
        putInt(b, at + 4, (int) (v >>> 32));
    }

    static int getInt(byte[] b, int at) {
        return (b[at] & 0xFF) | (b[at + 1] & 0xFF) << 8 | (b[at + 2] & 0xFF) << 16 | (b[at + 3] & 0xFF) << 24;
    }

    static long getLong(byte[] b, int at) {
        return (getInt(b, at) & 0xFFFFFFFFL) | (long) getInt(b, at + 4) << 32;
    }
}
