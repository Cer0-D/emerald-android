package com.cer0.emerald;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * The radio of local wireless play: what the 3DS's local wireless (UDS) was to
 * 3ds_port/src/3ds_local.c, over Bluetooth LE (android_port/src/android_uds.c
 * calls these, from the session's thread).
 *
 * A session's host advertises (manufacturer data, company 0xFFFF - the one
 * kept for testing - and "E3") where to reach it: its L2CAP channel (PSM),
 * how many are in the session and how many fit, and the session's beacon
 * data. Players scan for that and connect to the channel - unpaired
 * (insecure L2CAP), so nothing is asked of them. The host gives each one a
 * node id (its own is 1), tells everyone who is in the session after every
 * change, and relays frames between the others, as UDS did.
 *
 * On a channel, packets: u16 length (of what follows), u8 type, u8 from,
 * u8 to, then the payload.
 */
public final class LocalBluetooth {
    private static final String TAG = "Emerald";
    private static final int COMPANY = 0xFFFF;
    private static final byte MAGIC0 = 'E', MAGIC1 = '3';

    private static final int T_WELCOME = 1; // host -> new player: payload [id]
    private static final int T_STATUS = 2;  // host -> all: payload [mask lo, mask hi]
    private static final int T_DATA = 3;    // a frame, relayed by the host
    private static final int T_BYE = 4;

    private static final int STATUS_IDLE = 3, STATUS_HOST = 6, STATUS_CLIENT = 9;
    /* Heard this recently: still there (scans run on, see scan). */
    private static final long SEEN_MS = 2500;
    private static final int REQUEST_PERMISSIONS = 4242;

    private static Activity sActivity;
    private static final Object sLock = new Object();

    // Status (guarded by sLock).
    private static int sStatus = STATUS_IDLE;
    private static int sMyId;
    private static int sMask;
    private static int sMaxNodes = 5;
    private static boolean sStatusChanged;

    // Frames for the game: [from][payload].
    private static final ConcurrentLinkedQueue<byte[]> sIncoming = new ConcurrentLinkedQueue<>();

    // Host side.
    private static BluetoothServerSocket sServer;
    private static final Map<Integer, Conn> sChildren = new HashMap<>();
    private static byte[] sAdvertPayload = new byte[0];
    private static BluetoothLeAdvertiser sAdvertiser;
    private static boolean sAdvertising;

    // Client side.
    private static Conn sHost;
    /* Only to ask for a short connection interval (see connect). */
    private static BluetoothGatt sGatt;

    // Scanning: what was heard, by address.
    private static BluetoothLeScanner sScanner;
    private static boolean sScanning;
    private static long sScanStarted;
    private static final Map<String, Seen> sSeen = new HashMap<>();

    private static final class Seen {
        byte[] data;
        long when;
        int rssi;
    }

    static native void nativeWake();

    static void attach(Activity activity) {
        sActivity = activity;
    }

    private static BluetoothAdapter adapter() {
        if (sActivity == null) return null;
        BluetoothManager m = (BluetoothManager) sActivity.getSystemService(Context.BLUETOOTH_SERVICE);
        return m != null ? m.getAdapter() : null;
    }

    private static boolean granted(String permission) {
        return sActivity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    /** Whether the radio can be used now. When not, asks for what is missing
     *  (the permissions, Bluetooth on) and the session fails this time. */
    static boolean start() {
        if (sActivity == null) return false;
        String[] needed = {Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_ADVERTISE,
                           Manifest.permission.BLUETOOTH_CONNECT};
        List<String> missing = new ArrayList<>();
        for (String p : needed)
            if (!granted(p)) missing.add(p);
        if (!missing.isEmpty()) {
            Log.i(TAG, "local: asking for the Bluetooth permissions");
            final String[] ask = missing.toArray(new String[0]);
            sActivity.runOnUiThread(() -> sActivity.requestPermissions(ask, REQUEST_PERMISSIONS));
            return false;
        }
        BluetoothAdapter a = adapter();
        if (a == null) {
            Log.w(TAG, "local: no Bluetooth on this device");
            return false;
        }
        if (!a.isEnabled()) {
            Log.i(TAG, "local: Bluetooth is off, asking to turn it on");
            sActivity.runOnUiThread(() -> {
                try {
                    sActivity.startActivity(new android.content.Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
                } catch (Exception e) {
                    Log.w(TAG, "local: cannot ask to turn Bluetooth on", e);
                }
            });
            return false;
        }
        return true;
    }

    static void stop() {
        leave();
        stopScan();
    }

    // ── Scanning ─────────────────────────────────────────────────────────

    private static final ScanCallback sScanCallback = new ScanCallback() {
        @Override public void onScanResult(int type, ScanResult r) {
            if (r.getScanRecord() == null) return;
            byte[] d = r.getScanRecord().getManufacturerSpecificData(COMPANY);
            if (d == null || d.length < 6 || d[0] != MAGIC0 || d[1] != MAGIC1) return;
            synchronized (sSeen) {
                Seen s = sSeen.get(r.getDevice().getAddress());
                if (s == null) {
                    s = new Seen();
                    sSeen.put(r.getDevice().getAddress(), s);
                }
                s.data = d;
                s.when = SystemClock.elapsedRealtime();
                s.rssi = r.getRssi();
            }
        }

        @Override public void onScanFailed(int error) {
            Log.w(TAG, "local: scan failed (" + error + ")");
            sScanning = false;
        }
    };

    private static void startScan() {
        if (sScanning) return;
        BluetoothAdapter a = adapter();
        if (a == null || !a.isEnabled()) return;
        sScanner = a.getBluetoothLeScanner();
        if (sScanner == null) return;
        ScanFilter filter = new ScanFilter.Builder()
                .setManufacturerData(COMPANY, new byte[]{MAGIC0, MAGIC1}, new byte[]{(byte) 0xFF, (byte) 0xFF})
                .build();
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
                .build();
        try {
            sScanner.startScan(Collections.singletonList(filter), settings, sScanCallback);
            sScanning = true;
            sScanStarted = SystemClock.elapsedRealtime();
        } catch (SecurityException e) {
            Log.w(TAG, "local: no permission to scan", e);
        }
    }

    private static void stopScan() {
        if (!sScanning || sScanner == null) return;
        try {
            sScanner.stopScan(sScanCallback);
        } catch (Exception e) {
            Log.w(TAG, "local: stopping the scan", e);
        }
        sScanning = false;
    }

    /**
     * The sessions heard: per host, its address (17 chars), then its
     * manufacturer data. Android lets an app start a scan only five times in
     * thirty seconds (more are silently empty), and 3ds_local.c scans over and
     * over: one scan runs on, and each call returns what it heard lately,
     * waiting up to waitMs for a scan just started to hear something.
     */
    static byte[][] scan(int waitMs) {
        startScan();
        long until = sScanStarted + waitMs;
        long now = SystemClock.elapsedRealtime();
        if (until > now) SystemClock.sleep(Math.min(until - now, waitMs));
        List<byte[]> out = new ArrayList<>();
        now = SystemClock.elapsedRealtime();
        synchronized (sSeen) {
            for (Map.Entry<String, Seen> e : sSeen.entrySet()) {
                if (now - e.getValue().when > SEEN_MS) continue;
                byte[] addr = e.getKey().getBytes();
                byte[] d = e.getValue().data;
                byte[] rec = new byte[addr.length + d.length];
                System.arraycopy(addr, 0, rec, 0, addr.length);
                System.arraycopy(d, 0, rec, addr.length, d.length);
                out.add(rec);
            }
        }
        return out.toArray(new byte[0][]);
    }

    // ── Hosting ──────────────────────────────────────────────────────────

    /** Hosts a session of up to maxNodes, its beacon data `payload`. */
    static boolean host(int maxNodes, byte[] payload) {
        leave();
        BluetoothAdapter a = adapter();
        if (a == null) return false;
        try {
            sServer = a.listenUsingInsecureL2capChannel();
        } catch (IOException | SecurityException e) {
            Log.w(TAG, "local: cannot listen", e);
            return false;
        }
        synchronized (sLock) {
            sStatus = STATUS_HOST;
            sMyId = 1;
            sMask = 1;
            sMaxNodes = maxNodes;
            sStatusChanged = true;
            sAdvertPayload = payload != null ? payload : new byte[0];
        }
        final BluetoothServerSocket server = sServer;
        Thread t = new Thread(() -> acceptLoop(server), "local-accept");
        t.setDaemon(true);
        t.start();
        advertise();
        Log.i(TAG, "local: hosting on L2CAP channel " + server.getPsm());
        return true;
    }

    static void setAppData(byte[] payload) {
        synchronized (sLock) {
            sAdvertPayload = payload != null ? payload : new byte[0];
        }
        if (sStatus == STATUS_HOST) advertise();
    }

    private static final AdvertiseCallback sAdvertiseCallback = new AdvertiseCallback() {
        @Override public void onStartFailure(int error) {
            Log.w(TAG, "local: advertising failed (" + error + ")");
            sAdvertising = false;
        }
    };

    /** (Re)advertises the session: where to connect, who's in it, its data. */
    private static void advertise() {
        BluetoothAdapter a = adapter();
        if (a == null) return;
        if (sAdvertiser == null) sAdvertiser = a.getBluetoothLeAdvertiser();
        if (sAdvertiser == null) {
            Log.w(TAG, "local: this device cannot advertise");
            return;
        }
        byte[] d;
        synchronized (sLock) {
            if (sServer == null) return;
            int psm = sServer.getPsm();
            int total = Integer.bitCount(sMask);
            d = new byte[6 + sAdvertPayload.length];
            d[0] = MAGIC0;
            d[1] = MAGIC1;
            d[2] = (byte) psm;
            d[3] = (byte) (psm >> 8);
            d[4] = (byte) total;
            d[5] = (byte) sMaxNodes;
            System.arraycopy(sAdvertPayload, 0, d, 6, sAdvertPayload.length);
        }
        try {
            if (sAdvertising) sAdvertiser.stopAdvertising(sAdvertiseCallback);
            AdvertiseSettings settings = new AdvertiseSettings.Builder()
                    .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                    .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
                    .setConnectable(true)
                    .build();
            AdvertiseData data = new AdvertiseData.Builder()
                    .setIncludeDeviceName(false)
                    .addManufacturerData(COMPANY, d)
                    .build();
            sAdvertiser.startAdvertising(settings, data, sAdvertiseCallback);
            sAdvertising = true;
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "local: cannot advertise", e);
        }
    }

    private static void stopAdvertising() {
        if (!sAdvertising || sAdvertiser == null) return;
        try {
            sAdvertiser.stopAdvertising(sAdvertiseCallback);
        } catch (Exception e) {
            Log.w(TAG, "local: stopping the advertisement", e);
        }
        sAdvertising = false;
    }

    private static void acceptLoop(BluetoothServerSocket server) {
        while (true) {
            BluetoothSocket s;
            try {
                s = server.accept();
            } catch (IOException e) {
                return; // closed
            }
            int id = 0;
            synchronized (sLock) {
                if (sServer != server) {
                    closeQuietly(s);
                    return;
                }
                for (int i = 2; i <= sMaxNodes; i++)
                    if ((sMask & (1 << (i - 1))) == 0) {
                        id = i;
                        break;
                    }
            }
            if (id == 0) {
                Log.i(TAG, "local: session full, turned a player away");
                closeQuietly(s);
                continue;
            }
            Conn c;
            try {
                c = new Conn(s, id);
            } catch (IOException e) {
                closeQuietly(s);
                continue;
            }
            synchronized (sLock) {
                sChildren.put(id, c);
                sMask |= 1 << (id - 1);
                sStatusChanged = true;
            }
            c.send(T_WELCOME, 1, id, new byte[]{(byte) id});
            broadcastStatus();
            advertise();
            c.startReading();
            Log.i(TAG, "local: player " + id + " joined (" + s.getRemoteDevice().getAddress() + ")");
            nativeWake();
        }
    }

    private static void broadcastStatus() {
        List<Conn> all;
        byte[] mask;
        synchronized (sLock) {
            all = new ArrayList<>(sChildren.values());
            mask = new byte[]{(byte) sMask, (byte) (sMask >> 8)};
        }
        for (Conn c : all) c.send(T_STATUS, 1, c.id, mask);
    }

    static boolean eject(int id) {
        Conn c;
        synchronized (sLock) {
            c = sChildren.get(id);
        }
        if (c == null) return false;
        c.close();
        return true;
    }

    // ── Joining ──────────────────────────────────────────────────────────

    /** Joins the session hosted at address, on its channel psm. */
    static boolean connect(String address, int psm) {
        leave();
        BluetoothAdapter a = adapter();
        if (a == null) return false;
        stopScan(); // a scan running slows the connection down
        BluetoothSocket s = null;
        try {
            BluetoothDevice device = a.getRemoteDevice(address);
            s = device.createInsecureL2capChannel(psm);
            s.connect();
            Conn c = new Conn(s, 1);
            synchronized (sLock) {
                sHost = c;
                sStatus = STATUS_CLIENT;
                sMyId = 0;
                sMask = 0;
            }
            c.startReading();
            requestFastLink(device);
            // The host's welcome says who we are.
            long until = SystemClock.elapsedRealtime() + 5000;
            while (SystemClock.elapsedRealtime() < until) {
                synchronized (sLock) {
                    if (sMyId != 0) {
                        sStatusChanged = true;
                        Log.i(TAG, "local: joined " + address + " as player " + sMyId);
                        return true;
                    }
                    if (sHost != c) break;
                }
                SystemClock.sleep(10);
            }
            Log.w(TAG, "local: no welcome from " + address);
        } catch (IOException | SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "local: joining " + address + " failed: " + e.getMessage());
        }
        closeQuietly(s);
        leave();
        return false;
    }

    /**
     * The link's connection interval, which the side that connected sets:
     * Android's default (30-50 ms) is a long wait for every frame's ack. An
     * L2CAP channel cannot ask for a shorter one, a GATT connection over the
     * same link can (CONNECTION_PRIORITY_HIGH, 7.5-15 ms); it is kept open
     * for that alone until the session ends.
     */
    private static void requestFastLink(BluetoothDevice device) {
        try {
            sGatt = device.connectGatt(sActivity, false, new BluetoothGattCallback() {
                @Override public void onConnectionStateChange(BluetoothGatt gatt, int status, int state) {
                    if (state == BluetoothProfile.STATE_CONNECTED) {
                        boolean asked = gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                        Log.i(TAG, "local: fast link interval " + (asked ? "asked for" : "refused"));
                    }
                }
            }, BluetoothDevice.TRANSPORT_LE);
        } catch (SecurityException e) {
            Log.w(TAG, "local: cannot ask for a fast link", e);
        }
    }

    private static void closeGatt() {
        if (sGatt == null) return;
        try {
            sGatt.disconnect();
            sGatt.close();
        } catch (SecurityException ignored) {
        }
        sGatt = null;
    }

    // ── Both ─────────────────────────────────────────────────────────────

    static void leave() {
        List<Conn> all = new ArrayList<>();
        BluetoothServerSocket server;
        synchronized (sLock) {
            all.addAll(sChildren.values());
            if (sHost != null) all.add(sHost);
            sChildren.clear();
            sHost = null;
            server = sServer;
            sServer = null;
            boolean was = sStatus != STATUS_IDLE;
            sStatus = STATUS_IDLE;
            sMyId = 0;
            sMask = 0;
            if (was) sStatusChanged = true;
        }
        stopAdvertising();
        closeGatt();
        for (Conn c : all) {
            c.send(T_BYE, 0, 0, new byte[0]);
            c.close();
        }
        closeQuietly(server);
        sIncoming.clear();
    }

    /** {status, our id, the node mask, nodes in it, whether that changed}. */
    static int[] status() {
        synchronized (sLock) {
            int changed = sStatusChanged ? 1 : 0;
            sStatusChanged = false;
            return new int[]{sStatus, sMyId, sMask, Integer.bitCount(sMask), changed};
        }
    }

    /** A frame for node `to`, from us. */
    static boolean send(int to, byte[] frame) {
        Conn c;
        int from;
        synchronized (sLock) {
            from = sMyId;
            if (sStatus == STATUS_HOST) c = sChildren.get(to);
            else c = sHost; // the host delivers it, or relays it
        }
        return c != null && c.send(T_DATA, from, to, frame);
    }

    /** The next frame for us, [from][frame], or null. */
    static byte[] pull() {
        return sIncoming.poll();
    }

    private static void received(Conn c, int type, int from, int to, byte[] payload) {
        switch (type) {
        case T_WELCOME:
            if (payload.length >= 1) synchronized (sLock) {
                if (sHost == c) sMyId = payload[0] & 0xFF;
            }
            break;
        case T_STATUS:
            if (payload.length >= 2) synchronized (sLock) {
                if (sHost == c) {
                    sMask = (payload[0] & 0xFF) | ((payload[1] & 0xFF) << 8);
                    sStatusChanged = true;
                }
            }
            nativeWake();
            break;
        case T_DATA: {
            Conn relay = null;
            boolean mine;
            synchronized (sLock) {
                mine = to == sMyId;
                if (!mine && sStatus == STATUS_HOST) relay = sChildren.get(to);
            }
            if (mine) {
                byte[] item = new byte[payload.length + 1];
                item[0] = (byte) (sStatus == STATUS_HOST ? c.id : from);
                System.arraycopy(payload, 0, item, 1, payload.length);
                sIncoming.add(item);
                nativeWake();
            } else if (relay != null) {
                relay.send(T_DATA, c.id, to, payload);
            }
            break;
        }
        case T_BYE:
            c.close();
            break;
        }
    }

    private static void lost(Conn c) {
        boolean host;
        synchronized (sLock) {
            host = sStatus == STATUS_HOST;
            if (host) {
                if (sChildren.get(c.id) != c) return;
                sChildren.remove(c.id);
                sMask &= ~(1 << (c.id - 1));
                sStatusChanged = true;
            } else {
                if (sHost != c) return;
                sHost = null;
                sStatus = STATUS_IDLE;
                sMyId = 0;
                sMask = 0;
                sStatusChanged = true;
            }
        }
        Log.i(TAG, host ? "local: player " + c.id + " left" : "local: the host is gone");
        if (host) {
            broadcastStatus();
            advertise();
        }
        nativeWake();
    }

    private static void closeQuietly(java.io.Closeable c) {
        if (c == null) return;
        try {
            c.close();
        } catch (IOException ignored) {
        }
    }

    /** One L2CAP channel: to a player (host side) or to the host. */
    private static final class Conn {
        final BluetoothSocket socket;
        final int id; // the other side's node id
        final InputStream in;
        final OutputStream out;
        volatile boolean closed;

        Conn(BluetoothSocket socket, int id) throws IOException {
            this.socket = socket;
            this.id = id;
            this.in = socket.getInputStream();
            this.out = socket.getOutputStream();
        }

        void startReading() {
            Thread t = new Thread(this::readLoop, "local-read-" + id);
            t.setDaemon(true);
            t.start();
        }

        boolean send(int type, int from, int to, byte[] payload) {
            if (closed) return false;
            byte[] p = new byte[5 + payload.length];
            int len = 3 + payload.length;
            p[0] = (byte) len;
            p[1] = (byte) (len >> 8);
            p[2] = (byte) type;
            p[3] = (byte) from;
            p[4] = (byte) to;
            System.arraycopy(payload, 0, p, 5, payload.length);
            synchronized (this) {
                try {
                    out.write(p);
                    out.flush();
                    return true;
                } catch (IOException e) {
                    close();
                    return false;
                }
            }
        }

        private boolean readFully(byte[] b) throws IOException {
            int off = 0;
            while (off < b.length) {
                int n = in.read(b, off, b.length - off);
                if (n < 0) return false;
                off += n;
            }
            return true;
        }

        private void readLoop() {
            byte[] head = new byte[2];
            try {
                while (!closed && readFully(head)) {
                    int len = (head[0] & 0xFF) | ((head[1] & 0xFF) << 8);
                    if (len < 3 || len > 4096) break;
                    byte[] body = new byte[len];
                    if (!readFully(body)) break;
                    byte[] payload = new byte[len - 3];
                    System.arraycopy(body, 3, payload, 0, payload.length);
                    received(this, body[0] & 0xFF, body[1] & 0xFF, body[2] & 0xFF, payload);
                }
            } catch (IOException ignored) {
            }
            close();
        }

        void close() {
            if (closed) return;
            closed = true;
            closeQuietly(socket);
            lost(this);
        }
    }
}
