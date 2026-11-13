package com.cer0.emerald;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The follower's files from HeartGold/SoulSilver's: builder/emerald3ds_builder/
 * followers.py's conversions in Java, byte for byte (its docstring has the
 * FMN1 and FMT1 formats). No Android classes: FollowerFetcher downloads, this
 * converts.
 */
final class FollowerFiles {
    static final class FormatException extends Exception {
        FormatException(String message) { super(message); }
    }

    static final int FRAMES = 8;
    static final int TEXT_BANK = 265;
    static final int TEXT_COUNT = 759;

    private FollowerFiles() {}

    private static int u16(byte[] b, int at) { return (b[at] & 0xFF) | (b[at + 1] & 0xFF) << 8; }
    private static int u32(byte[] b, int at) { return u16(b, at) | u16(b, at + 2) << 16; }

    private static byte[] slice(byte[] b, int from, int to) {
        from = Math.max(0, Math.min(from, b.length));
        to = Math.max(from, Math.min(to, b.length));
        byte[] out = new byte[to - from];
        System.arraycopy(b, from, out, 0, out.length);
        return out;
    }

    // ── NSBTX ────────────────────────────────────────────────────────────

    /** A Nitro info dictionary's entries, in order. */
    private static List<byte[]> dictionary(byte[] buf, int off) {
        int count = buf[off + 1] & 0xFF;
        int p = off + 4 + 8 + 4 * count;
        int size = u16(buf, p);
        p += 4;
        List<byte[]> entries = new ArrayList<>();
        for (int i = 0; i < count; i++) entries.add(slice(buf, p + i * size, p + (i + 1) * size));
        return entries;
    }

    /** A 4bpp linear texture (low nibble = left pixel, as the GBA's) in 8x8 tiles. */
    static byte[] gbaTiles(byte[] linear, int w, int h) {
        int row = w / 2;
        ByteArrayOutputStream out = new ByteArrayOutputStream(linear.length);
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < row; tx += 4)
                for (int y = ty; y < ty + 8; y++) out.write(linear, y * row + tx, 4);
        return out.toByteArray();
    }

    /** One NSBTX (8 frames, the normal and the shiny palette) as an FMN1 file. */
    static byte[] convert(byte[] buf, int flags) throws FormatException {
        try {
            if (buf.length < 4 || !new String(buf, 0, 4, StandardCharsets.ISO_8859_1).equals("BTX0"))
                throw new FormatException("not a BTX0 file");
            int t = u32(buf, 0x10);
            if (!new String(buf, t, 4, StandardCharsets.ISO_8859_1).equals("TEX0"))
                throw new FormatException("no TEX0 block");
            int texInfo = u16(buf, t + 0x0E);
            int texData = u32(buf, t + 0x14);
            int palSize = u32(buf, t + 0x30) << 3;
            int palInfo = u32(buf, t + 0x34);
            int palData = u32(buf, t + 0x38);

            List<byte[]> textures = dictionary(buf, t + texInfo);
            List<byte[]> palettes = dictionary(buf, t + palInfo);
            if (textures.size() != FRAMES || palettes.size() < 2)
                throw new FormatException(textures.size() + " textures, " + palettes.size() + " palettes");
            int w = 0, h = 0;
            byte[][] frames = new byte[FRAMES][];
            for (int i = 0; i < FRAMES; i++) {
                int param = u32(textures.get(i), 0);
                int tw = 8 << ((param >>> 20) & 7), th = 8 << ((param >>> 23) & 7);
                int fmt = (param >>> 26) & 7, t0 = (param >>> 29) & 1;
                int start = t + texData + ((param & 0xFFFF) << 3);
                byte[] data = slice(buf, start, start + tw * th / 2);
                if (i == 0) { w = tw; h = th; }
                if (tw != w || th != h || fmt != 3 || t0 != 1 || data.length != w * h / 2)
                    throw new FormatException("unexpected texture " + tw + "x" + th + " format " + fmt);
                frames[i] = data;
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream(72 + FRAMES * w * h / 2);
            out.write('F');
            out.write('M');
            out.write('N');
            out.write('1');
            out.write(w);
            out.write(h);
            out.write(FRAMES);
            out.write(flags);
            byte[][] colours = new byte[palettes.size()][];
            for (int k = 0; k < colours.length; k++) {
                int start = t + palData + (u16(palettes.get(k), 0) << 3);
                colours[k] = slice(buf, start, t + palData + palSize);
                if (colours[k].length < 32) throw new FormatException("short palette");
            }
            for (int k = 0; k < 2; k++) {
                byte[] raw = colours[k];
                for (int c = 0; c < 16; c++) {
                    int colour = u16(raw, c * 2) & 0x7FFF;
                    out.write(colour & 0xFF);
                    out.write(colour >> 8);
                }
            }
            for (byte[] f : frames) {
                byte[] tiles = gbaTiles(f, w, h);
                out.write(tiles, 0, tiles.length);
            }
            return out.toByteArray();
        } catch (IndexOutOfBoundsException e) {
            throw new FormatException("truncated file");
        }
    }

    /** tp_param.csv, a row per follower index: too tall, column 3, hover. */
    static int[] paramFlags(String csv) {
        String[] lines = csv.split("\r\n|\r|\n", -1);
        List<Integer> flags = new ArrayList<>();
        for (int i = 1; i < lines.length; i++) {
            String[] cols = lines[i].strip().split(",", -1);
            if (cols.length >= 5)
                flags.add((cols[2].equals("true") ? 1 : 0) | (cols[3].equals("true") ? 2 : 0)
                        | (cols[4].equals("true") ? 4 : 0));
        }
        int[] out = new int[flags.size()];
        for (int i = 0; i < out.length; i++) out[i] = flags.get(i);
        return out;
    }

    // ── Texts ────────────────────────────────────────────────────────────

    private static final Map<Integer, Integer> CHARS = new HashMap<>();
    static {
        Object[] pairs = {
            " ", 0x00, "Ç", 0x04, "È", 0x05, "à", 0x16, "á", 0x17, "ç", 0x19, "è", 0x1A, "é", 0x1B,
            "ê", 0x1C, "î", 0x20, "ò", 0x22, "ó", 0x23, "ô", 0x24, "œ", 0x25, "ù", 0x26, "ú", 0x27,
            "û", 0x28, "ñ", 0x29, "¿", 0x51, "¡", 0x52, "â", 0x68, "í", 0x6F, "!", 0xAB, "?", 0xAC,
            ".", 0xAD, "-", 0xAE, "'", 0xB4, "’", 0xB4, ",", 0xB8, ":", 0xF0, "ü", 0xF6,
            "ß", 0x15, "Ä", 0xF1, "Ö", 0xF2, "Ü", 0xF3, "ä", 0xF4, "ö", 0xF5,
        };
        for (int i = 0; i < pairs.length; i += 2)
            CHARS.put(((String) pairs[i]).codePointAt(0), (Integer) pairs[i + 1]);
    }
    private static final Pattern TOKEN =
            Pattern.compile("\\[VAR ([0-9A-F]{4})\\([^\\]]*\\)\\]|\\\\(.)|(.)", Pattern.DOTALL);

    /** One corpus line in the game's character set, ending in 0xFF. */
    static byte[] encodeMessage(String text) throws FormatException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(text.length() + 1);
        Matcher m = TOKEN.matcher(text);
        while (m.find()) {
            String var = m.group(1), esc = m.group(2), ch = m.group(3);
            if (var != null) {
                switch (var) {
                    case "0101": out.write(0xFD); out.write(0x02); break;
                    case "0103": out.write(0xFD); out.write(0x01); break;
                    case "0108": out.write(0xFD); out.write(0x03); break;
                    case "0200": break;
                    default: throw new FormatException("unknown text variable " + var);
                }
            } else if (esc != null) {
                out.write(esc.equals("c") ? 0xFB : 0x00);
            } else {
                int c = ch.codePointAt(0);
                if (c >= '0' && c <= '9') out.write(0xA1 + c - '0');
                else if (c >= 'A' && c <= 'Z') out.write(0xBB + c - 'A');
                else if (c >= 'a' && c <= 'z') out.write(0xD5 + c - 'a');
                else out.write(CHARS.getOrDefault(c, 0xAC));
            }
        }
        byte[] raw = out.toByteArray();
        // No space next to a page break, nor two in a row, none at either end.
        ByteArrayOutputStream a = new ByteArrayOutputStream(raw.length);
        for (int i = 0; i < raw.length; ) {
            if (raw[i] == 0 || raw[i] == (byte) 0xFB) {
                int j = i;
                while (j < raw.length && raw[j] == 0) j++;
                if (j < raw.length && raw[j] == (byte) 0xFB) {
                    j++;
                    while (j < raw.length && raw[j] == 0) j++;
                    a.write(0xFB);
                    i = j;
                    continue;
                }
            }
            a.write(raw[i++]);
        }
        byte[] b = a.toByteArray();
        ByteArrayOutputStream c = new ByteArrayOutputStream(b.length + 1);
        for (int i = 0; i < b.length; i++)
            if (b[i] != 0 || i == 0 || b[i - 1] != 0) c.write(b[i]);
        byte[] d = c.toByteArray();
        int from = 0, to = d.length;
        while (from < to && d[from] == 0) from++;
        while (to > from && d[to - 1] == 0) to--;
        byte[] result = new byte[to - from + 1];
        System.arraycopy(d, from, result, 0, to - from);
        result[to - from] = (byte) 0xFF;
        return result;
    }

    /** A poke-corpus *_msg.txt's bank 265 as an FMT1 file. */
    static byte[] convertTexts(String corpus) throws FormatException {
        String[] lines = corpus.replace("\r\n", "\n").split("\n", -1);
        int start = -1;
        for (int i = 0; i < lines.length; i++)
            if (lines[i].equals("Text File : " + TEXT_BANK)) { start = i + 2; break; }
        if (start < 0) throw new FormatException("bank " + TEXT_BANK + " is not in the text");
        int end = start;
        while (end < lines.length && !lines[end].startsWith("~~~")) end++;
        if (end - start != TEXT_COUNT)
            throw new FormatException("bank " + TEXT_BANK + " has " + (end - start) + " messages, not " + TEXT_COUNT);
        byte[][] blobs = new byte[TEXT_COUNT][];
        int total = 0;
        for (int i = 0; i < TEXT_COUNT; i++) {
            blobs[i] = encodeMessage(lines[start + i]);
            total += blobs[i].length;
        }
        ByteBuffer out = ByteBuffer.allocate(8 + 4 * TEXT_COUNT + total).order(ByteOrder.LITTLE_ENDIAN);
        out.put("FMT1".getBytes(StandardCharsets.US_ASCII));
        out.putShort((short) TEXT_COUNT);
        out.putShort((short) 0);
        int at = 8 + 4 * TEXT_COUNT;
        for (byte[] b : blobs) {
            out.putInt(at);
            at += b.length;
        }
        for (byte[] b : blobs) out.put(b);
        return out.array();
    }
}
