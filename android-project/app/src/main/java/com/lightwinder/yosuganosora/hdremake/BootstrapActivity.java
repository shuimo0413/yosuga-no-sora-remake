package com.lightwinder.yosuganosora.hdremake;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Android bootstrap: detects the external game data (Download/YosugaSoraHD
 * or Android/data/<pkg>), offers download / import, extracts zips and
 * data.xp3 archives, then starts the engine activity. Mirrors the OHOS
 * shell behaviour (including the one-time re-import prompt after an app
 * update).
 */
public class BootstrapActivity extends Activity {
    private static final String TAG = "YosugaBootstrap";
    private static final String PREFS = "data_setup";
    private static final String KEY_CONFIRMED_VERSION = "confirmed_version";
    private static final String DEFAULT_BASE_URL =
            "https://github.com/WarSkyGod/yosuga-no-sora-remake/releases/latest/download/";

    private LinearLayout root;
    private TextView messageView;
    private TextView progressView;
    private ProgressBar progressBar;
    private EditText baseUrlInput;
    private EditText proxyInput;
    private Button downloadButton;
    private Button importButton;
    private boolean busy = false;

    private static final int STORAGE_PERMISSION_REQUEST = 9001;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        // Build the content view FIRST: the immersive setup below touches
        // the window insets controller, which NPEs on some Android 16
        // environments (卓易通) when the DecorView does not exist yet.
        buildUi();
        applyImmersive();
        requestStoragePermissionIfNeeded();
        probeData();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersive();
        // Returning from the system Settings screen (MANAGE_EXTERNAL_STORAGE
        // on Android 11+) may have just granted public storage: re-probe so
        // a ready data tree starts the game directly.
        if (!busy) probeData();
    }

    /** Storage permission for the public Downloads write (mirrors the engine
     * activity): WRITE/READ pair on Android 10-, MANAGE_EXTERNAL_STORAGE via
     * system Settings on Android 11+. chooseDataParent() probes the actual
     * writability and falls back to Android/data/<pkg> when it is missing. */
    private void requestStoragePermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                try {
                    startActivity(new Intent(
                            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + getPackageName())));
                } catch (Exception ignored) {
                    try {
                        startActivity(new Intent(
                                Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    } catch (Exception ignored2) {
                        // No settings screen available; stay in the private dir.
                    }
                }
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{
                        android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                        android.Manifest.permission.READ_EXTERNAL_STORAGE
                }, STORAGE_PERMISSION_REQUEST);
            }
        }
    }

    /** Fullscreen immersive: hide the status/navigation bars (including the
     * gesture pill area) so the bootstrap page has no bottom black strip. */
    private void applyImmersive() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                getWindow().setDecorFitsSystemWindows(false);
                android.view.WindowInsetsController controller =
                        getWindow().getInsetsController();
                if (controller != null) {
                    controller.hide(android.view.WindowInsets.Type.statusBars()
                            | android.view.WindowInsets.Type.navigationBars());
                    controller.setSystemBarsBehavior(
                            android.view.WindowInsetsController
                                    .BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            } else {
                getWindow().getDecorView().setSystemUiVisibility(
                        android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                        | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            }
        } catch (Throwable t) {
            // Immersion is cosmetic: never crash the bootstrap over it.
        }
    }

    // ---- UI -----------------------------------------------------------------
    private void buildUi() {
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(48, 48, 48, 48);
        root.setBackgroundResource(R.drawable.background);

        TextView title = new TextView(this);
        title.setText("缘之空：高清重制");
        title.setTextSize(26f);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        title.setShadowLayer(3f, 0f, 0f, Color.BLACK);

        messageView = new TextView(this);
        messageView.setText(" ");
        messageView.setTextSize(11f);
        messageView.setTextColor(Color.RED);
        messageView.setGravity(Gravity.CENTER);

        TextView hint = new TextView(this);
        hint.setText("需要游戏数据（约 3.6 GB）：可在线下载，或从本地选择 zip 压缩包 / data.xp3 导入");
        hint.setTextSize(13f);
        hint.setTextColor(Color.BLACK);
        hint.setGravity(Gravity.CENTER);

        baseUrlInput = new EditText(this);
        baseUrlInput.setText("");
        baseUrlInput.setTextSize(12f);
        baseUrlInput.setSingleLine(true);
        baseUrlInput.setHint("下载地址（留空使用默认值）");

        proxyInput = new EditText(this);
        proxyInput.setText("");
        proxyInput.setTextSize(12f);
        proxyInput.setSingleLine(true);
        proxyInput.setHint("加速代理前缀（留空=直连）");

        LinearLayout proxyButtons = new LinearLayout(this);
        proxyButtons.setOrientation(LinearLayout.HORIZONTAL);
        proxyButtons.setGravity(Gravity.CENTER);
        Button directBtn = new Button(this);
        directBtn.setText("直连");
        directBtn.setTextColor(Color.BLACK);
        directBtn.setOnClickListener(v -> proxyInput.setText(""));
        Button ghBtn = new Button(this);
        ghBtn.setText("gh-proxy");
        ghBtn.setTextColor(Color.BLACK);
        ghBtn.setOnClickListener(v -> proxyInput.setText("https://gh-proxy.cn/"));
        Button craftBtn = new Button(this);
        craftBtn.setText("Craft-Hello Proxy");
        craftBtn.setTextColor(Color.BLACK);
        craftBtn.setOnClickListener(v -> proxyInput.setText("https://proxy.craft-hello.top/proxy/"));
        proxyButtons.addView(directBtn);
        proxyButtons.addView(ghBtn);
        proxyButtons.addView(craftBtn);

        TextView hint2 = new TextView(this);
        hint2.setText("下载地址留空使用默认值；加速代理前缀会自动拼在原下载链接前");
        hint2.setTextSize(11f);
        hint2.setTextColor(Color.BLACK);
        hint2.setGravity(Gravity.CENTER);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        downloadButton = new Button(this);
        downloadButton.setText("下载游戏数据");
        downloadButton.setTextColor(Color.BLACK);
        downloadButton.setOnClickListener(v -> startDownload());
        importButton = new Button(this);
        importButton.setText("从本地压缩包导入");
        importButton.setTextColor(Color.BLACK);
        importButton.setOnClickListener(v -> startImport());
        LinearLayout.LayoutParams btnLp = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        buttons.addView(downloadButton, btnLp);
        buttons.addView(importButton, btnLp);

        progressView = new TextView(this);
        progressView.setText("");
        progressView.setTextSize(14f);
        progressView.setTextColor(Color.WHITE);
        progressView.setGravity(Gravity.CENTER);
        progressView.setBackgroundColor(0xFF333333);
        progressView.setPadding(12, 12, 12, 12);

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(100);

        root.addView(title);
        root.addView(messageView);
        root.addView(hint);
        root.addView(baseUrlInput);
        root.addView(proxyInput);
        root.addView(proxyButtons);
        root.addView(hint2);
        root.addView(buttons);
        root.addView(progressView);
        root.addView(progressBar);
        setContentView(root);
    }

    private void setProgress(String text, int percent) {
        runOnUiThread(() -> {
            progressView.setText(text);
            progressBar.setProgress(percent);
        });
    }

    private void setMessage(String text) {
        runOnUiThread(() -> messageView.setText(text));
    }

    private void setBusy(boolean value) {
        busy = value;
        runOnUiThread(() -> {
            downloadButton.setEnabled(!value);
            importButton.setEnabled(!value);
            // Keep the screen ON while downloading / extracting so the
            // device does not go to sleep mid-transfer.
            if (value) {
                getWindow().addFlags(
                        android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            } else {
                getWindow().clearFlags(
                        android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
        });
    }

    // ---- data location ------------------------------------------------------
    private File downloadRoot() {
        return new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS), "YosugaSoraHD");
    }

    private File appExternalRoot() {
        File ext = getExternalFilesDir(null);
        return ext != null ? ext.getParentFile() : null; // Android/data/<pkg>
    }

    private boolean dataReady(File dataDir) {
        if (dataDir == null) return false;
        return new File(dataDir, "startup.tjs").isFile()
                && new File(dataDir, "system").isDirectory();
    }

    /** First location that contains a ready data/ tree. */
    private File findReadyDataDir() {
        File d = new File(downloadRoot(), "data");
        if (dataReady(d)) return d;
        File p = appExternalRoot();
        if (p != null) {
            File d2 = new File(p, "data");
            if (dataReady(d2)) return d2;
        }
        return null;
    }

    /**
     * Pick the writable data parent: the public Download/YosugaSoraHD folder
     * when the platform allows writing there, otherwise Android/data/<pkg>.
     */
    private File chooseDataParent() {
        File dl = downloadRoot();
        try {
            if (!dl.exists() && !dl.mkdirs()) throw new IOException("mkdirs failed");
            File probe = new File(dl, ".probe");
            if (!probe.createNewFile() && !probe.exists()) throw new IOException("probe failed");
            probe.delete();
            return dl;
        } catch (Exception e) {
            Log.w(TAG, "public Download not writable; using app-external dir", e);
            File p = appExternalRoot();
            return p; // may be null; caller falls back to filesDir
        }
    }

    // ---- probe / confirm ----------------------------------------------------
    private void probeData() {
        File ready = findReadyDataDir();
        if (ready == null) {
            // Bundled install (old APK with assets): let the engine read assets.
            showSetup("");
            return;
        }
        File parent = chooseDataParent();
        if (parent != null) {
            ensureNoMedia(parent);
        }
        maybeConfirmUpdate(ready);
    }

    private void ensureNoMedia(File parent) {
        File marker = new File(parent, ".nomedia");
        if (!marker.exists()) {
            try { marker.createNewFile(); } catch (IOException ignored) {}
        }
    }

    /** After an app update, ask ONCE whether to re-import the game package. */
    private void maybeConfirmUpdate(File readyDir) {
        try {
            int version = getVersionCode();
            SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
            if (prefs.getInt(KEY_CONFIRMED_VERSION, -1) == version) {
                startEngine(readyDir);
                return;
            }
            new AlertDialog.Builder(this)
                .setTitle("检测到已有游戏数据")
                .setMessage("是否重新导入游戏包？\n“重新导入”将删除现有数据并跳转到导入页面；“使用旧数据”将直接启动。")
                .setPositiveButton("重新导入", (d, w) -> {
                    prefs.edit().putInt(KEY_CONFIRMED_VERSION, version).apply();
                    dropOldData();
                    showSetup("请重新导入游戏包");
                })
                .setNegativeButton("使用旧数据", (d, w) -> {
                    prefs.edit().putInt(KEY_CONFIRMED_VERSION, version).apply();
                    startEngine(readyDir);
                })
                .setCancelable(false)
                .show();
        } catch (Exception e) {
            startEngine(readyDir);
        }
    }

    private int getVersionCode() throws PackageManager.NameNotFoundException {
        PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
        return (int) info.getLongVersionCode();
    }

    private void dropOldData() {
        for (File dir : new File[]{new File(downloadRoot(), "data"),
                appExternalRoot() != null ? new File(appExternalRoot(), "data") : null}) {
            if (dir != null) deleteTree(dir);
        }
    }

    private void showSetup(String message) {
        setMessage(message);
        setProgress("", 0);
    }

    // ---- engine handoff -----------------------------------------------------
    private void startEngine(File dataDir) {
        setProgress("正在启动游戏…", 100);
        Intent intent = new Intent(this, KirikiriSDL2Activity.class);
        intent.putExtra("dataDir", dataDir != null ? dataDir.getAbsolutePath() : "");
        startActivity(intent);
        finish();
    }

    // ---- download -----------------------------------------------------------
    private void startDownload() {
        if (busy) return;
        setBusy(true);
        setMessage("");
        setProgress("正在获取下载清单…", 0);
        new Thread(() -> {
            try {
                List<String[]> assets = loadManifest();
                if (assets.isEmpty()) {
                    fail("无法读取下载清单（data-assets.json），请检查网络后重试");
                    return;
                }
                File parent = chooseDataParent();
                if (parent == null) {
                    fail("无法使用外部存储，无法下载数据");
                    return;
                }
                ensureNoMedia(parent);
                DataExtractService.start(this);
                try {
                    // Extract the zips STRAIGHT into the data directory (no
                    // staging dir, no cross-volume move).
                    File dataDir = new File(parent, "data");
                    if (dataDir.exists()) deleteTree(dataDir);
                    long total = 0;
                    for (String[] a : assets) total += Long.parseLong(a[2]);
                    long done = 0;
                    int index = 0;
                    for (String[] asset : assets) {
                        index++;
                        String name = asset[0];
                        String sha = asset[1];
                        long size = Long.parseLong(asset[2]);
                        File zip = new File(getCacheDir(), name);
                        downloadFile(asset[3], zip, size, done, total, name);
                        verifySha(zip, sha);
                        extractZipTo(zip, dataDir, "解压 " + name);
                        zip.delete();
                        done += size;
                    }
                    installIntoDataDir(dataDir, parent);
                } finally {
                    DataExtractService.stop(this);
                }
            } catch (Exception e) {
                Log.e(TAG, "download failed", e);
                fail("下载失败：" + e.getMessage());
            } finally {
                setBusy(false);
            }
        }).start();
    }

    /** Returns [name, sha256, size, url] tuples. The accelerator proxy
     * prefix (when set) is prepended to every asset URL, mirroring the
     * OHOS downloader. */
    private List<String[]> loadManifest() throws Exception {
        List<String[]> out = new ArrayList<>();
        String base = baseUrlInput.getText().toString().trim();
        if (base.isEmpty()) base = DEFAULT_BASE_URL;
        if (!base.endsWith("/")) base += "/";
        final String proxy = proxyInput.getText().toString().trim();
        String manifestUrl = proxy.isEmpty() ? (base + "data-assets.json")
                : (proxy + base + "data-assets.json");
        HttpURLConnection conn = (HttpURLConnection) new URL(manifestUrl).openConnection();
        conn.setConnectTimeout(20000);
        conn.setReadTimeout(30000);
        conn.setRequestProperty("User-Agent", "YosugaSoraHD/1.0");
        StringBuilder sb = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(
                conn.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
        } finally {
            conn.disconnect();
        }
        JSONObject rootObj = new JSONObject(sb.toString());
        JSONArray assets = rootObj.optJSONArray("assets");
        if (assets == null) return out;
        for (int i = 0; i < assets.length(); i++) {
            JSONObject a = assets.getJSONObject(i);
            String assetUrl = proxy.isEmpty() ? (base + a.getString("name"))
                    : (proxy + base + a.getString("name"));
            out.add(new String[]{
                a.getString("name"),
                a.optString("sha256", ""),
                String.valueOf(a.optLong("size", 0)),
                assetUrl,
            });
        }
        return out;
    }

    private void downloadFile(String urlStr, File dest, long size,
            long doneBase, long total, String label) throws IOException {
        HttpURLConnection conn = (HttpURLConnection) new URL(urlStr).openConnection();
        conn.setConnectTimeout(20000);
        conn.setReadTimeout(60000);
        conn.setRequestProperty("User-Agent", "YosugaSoraHD/1.0");
        long done = 0;
        try (InputStream in = new BufferedInputStream(conn.getInputStream());
             OutputStream out = new BufferedOutputStream(new FileOutputStream(dest))) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                done += n;
                int pct = total > 0 ? (int) ((doneBase + done) * 100 / total) : 0;
                setProgress(String.format(Locale.US, "正在下载 %s  %.1f%%", label,
                        total > 0 ? (doneBase + done) * 100.0 / total : 0), Math.min(99, pct));
            }
        } finally {
            conn.disconnect();
        }
    }

    private void verifySha(File file, String expectedSha) throws IOException {
        if (expectedSha == null || expectedSha.isEmpty()) return;
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (Exception e) {
            return;
        }
        try (InputStream in = new FileInputStream(file)) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) digest.update(buf, 0, n);
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format("%02x", b));
        if (!hex.toString().equalsIgnoreCase(expectedSha)) {
            throw new IOException("SHA-256 校验失败，请重试");
        }
    }

    // ---- import -------------------------------------------------------------
    private void startImport() {
        if (busy) return;
        setBusy(true);
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, 1001);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != 1001 || resultCode != RESULT_OK || data == null) {
            setBusy(false);
            return;
        }
        List<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) {
            for (int i = 0; i < data.getClipData().getItemCount(); i++) {
                uris.add(data.getClipData().getItemAt(i).getUri());
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        if (uris.isEmpty()) {
            setBusy(false);
            return;
        }
        final List<Uri> selected = uris;
        new Thread(() -> {
            try {
                handleImport(selected);
            } catch (Exception e) {
                Log.e(TAG, "import failed", e);
                fail("导入失败：" + e.getMessage());
            } finally {
                setBusy(false);
            }
        }).start();
    }

    private void handleImport(List<Uri> uris) throws Exception {
        File parent = chooseDataParent();
        if (parent == null) {
            fail("无法使用外部存储，无法导入数据");
            return;
        }
        ensureNoMedia(parent);
        List<Uri> zips = new ArrayList<>();
        for (Uri uri : uris) {
            String name = queryName(uri);
            String lower = name == null ? "" : name.toLowerCase(Locale.US);
            if (lower.endsWith(".xp3")) {
                setProgress("正在导入 " + name, 0);
                File xp3 = new File(parent, "data.xp3");
                copyUri(uri, xp3);
                extractXp3(xp3);
                return;
            } else if (lower.endsWith(".zip")) {
                zips.add(uri);
            } else {
                setMessage("不支持的文件类型：" + name);
            }
        }
        if (zips.isEmpty()) {
            fail("未选择有效的 zip 压缩包或 data.xp3 文件");
            return;
        }
        DataExtractService.start(this);
        try {
            // Extract the zips STRAIGHT into the data directory (no staging
            // dir, no cross-volume move).
            File dataDir = new File(parent, "data");
            if (dataDir.exists()) deleteTree(dataDir);
            for (int i = 0; i < zips.size(); i++) {
                String name = queryName(zips.get(i));
                setProgress("正在解压 " + name + "（" + (i + 1) + "/" + zips.size() + "）",
                        i * 100 / zips.size());
                File zip = new File(getCacheDir(), name);
                copyUri(zips.get(i), zip);
                try {
                    extractZipTo(zip, dataDir, "解压 " + name);
                } catch (IOException badZip) {
                    throw new IOException(name + " 不是有效的 zip 压缩包，请重新选择");
                }
                zip.delete();
            }
            installIntoDataDir(dataDir, parent);
        } finally {
            DataExtractService.stop(this);
        }
    }

    private String queryName(Uri uri) {
        String name = null;
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) name = c.getString(idx);
            }
        } catch (Exception ignored) {}
        if (name == null || name.isEmpty()) {
            String last = uri.getLastPathSegment();
            name = last != null ? last : "import";
        }
        return name;
    }

    private void copyUri(Uri uri, File dest) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new BufferedOutputStream(new FileOutputStream(dest))) {
            if (in == null) throw new IOException("无法读取所选文件");
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    // ---- extraction / install ----------------------------------------------
    private void extractZipTo(File zip, File outDir, String label) throws IOException {
        try (ZipFile zf = new ZipFile(zip)) {
            int total = 0;
            for (Enumeration<? extends ZipEntry> en = zf.entries(); en.hasMoreElements();) {
                if (!en.nextElement().isDirectory()) total++;
            }
            int done = 0;
            byte[] buf = new byte[1 << 20];
            for (Enumeration<? extends ZipEntry> en = zf.entries(); en.hasMoreElements();) {
                ZipEntry e = en.nextElement();
                if (e.isDirectory()) continue;
                String rel = e.getName().replace('\\', '/');
                if (rel.startsWith("data/")) rel = rel.substring(5);
                if (rel.isEmpty() || rel.startsWith("../") || rel.contains("/../")) continue;
                File out = new File(outDir, rel);
                if (out.getParentFile() != null && !out.getParentFile().exists()
                        && !out.getParentFile().mkdirs()) {
                    throw new IOException("无法创建目录：" + out.getParentFile());
                }
                try (InputStream in = zf.getInputStream(e);
                     OutputStream fo = new BufferedOutputStream(new FileOutputStream(out))) {
                    int n;
                    while ((n = in.read(buf)) > 0) fo.write(buf, 0, n);
                }
                done++;
                if (done % 512 == 0 || done == total) {
                    setProgress(label + "：" + done + " / " + total,
                            total > 0 ? done * 100 / total : 0);
                }
            }
        }
    }

    /** Finish a straight-into-data-dir extraction: verify the tree, or hand
     *  a zip-contained data.xp3 over to the native extractor. */
    private void installIntoDataDir(File dataDir, File parent) throws IOException {
        File innerXp3 = new File(dataDir, "data.xp3");
        if (innerXp3.isFile()) {
            // zip contained data.xp3: move it to the public root, then extract.
            setProgress("正在复制 data.xp3 到下载目录…", 0);
            File dst = new File(parent, "data.xp3");
            dst.delete();
            if (!innerXp3.renameTo(dst)) {
                copyFile(innerXp3, dst);
                innerXp3.delete();
            }
            deleteTree(dataDir);
            extractXp3(dst);
        } else if (new File(dataDir, "startup.tjs").isFile()) {
            markConfirmed();
            final File ready = dataDir;
            runOnUiThread(() -> {
                if (dataReady(ready)) startEngine(ready);
                else fail("数据安装后仍不可用，请重新导入");
            });
        } else {
            throw new IOException("压缩包内容不正确：未找到 data 文件夹或 data.xp3");
        }
    }

    private void copyFile(File src, File dst) throws IOException {
        try (InputStream in = new FileInputStream(src);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private void copyTree(File src, File dst) throws IOException {
        if (src.isDirectory()) {
            if (!dst.exists() && !dst.mkdirs()) throw new IOException("mkdirs failed: " + dst);
            String[] children = src.list();
            if (children == null) return;
            for (String child : children) copyTree(new File(src, child), new File(dst, child));
        } else {
            try (InputStream in = new FileInputStream(src);
                 OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[1 << 20];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
        }
    }

    private void markConfirmed() {
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt(KEY_CONFIRMED_VERSION, getVersionCode()).apply();
        } catch (Exception ignored) {}
    }

    // ---- xp3 extraction -----------------------------------------------------
    private void extractXp3(File xp3) throws IOException {
        File parent = chooseDataParent();
        if (parent == null) throw new IOException("无法使用外部存储");
        File tmp = new File(parent, "data.extract.tmp");
        File dataDir = new File(parent, "data");
        deleteTree(tmp);
        DataExtractService.start(this);
        try {
            setProgress("正在解包 data.xp3…", 0);
            boolean started = nativeExtractXp3Start(xp3.getAbsolutePath(), tmp.getAbsolutePath());
            if (!started) throw new IOException("无法启动解包线程");
            while (true) {
                sleepQuietly(400);
                String status = readText(new File(tmp.getAbsolutePath() + ".status"));
                if (status != null && !status.isEmpty()) {
                    status = status.trim();
                    if (status.startsWith("ok")) break;
                    String err = status.startsWith("error") ? status.substring(5).trim() : status;
                    throw new IOException("解包失败：" + (err.isEmpty() ? "未知错误" : err));
                }
                String progress = readText(new File(tmp.getAbsolutePath() + ".progress"));
                if (progress != null && !progress.isEmpty()) {
                    String[] parts = progress.trim().split(" ");
                    if (parts.length >= 2) {
                        try {
                            int done = Integer.parseInt(parts[0]);
                            int total = Integer.parseInt(parts[1]);
                            setProgress("正在解包 data.xp3：" + done + " / " + total,
                                    total > 0 ? done * 100 / total : 0);
                        } catch (NumberFormatException ignored) {}
                    }
                }
            }
            if (dataDir.exists()) deleteTree(dataDir);
            if (!tmp.renameTo(dataDir)) {
                setProgress("正在移动数据到数据目录…", 100);
                copyTree(tmp, dataDir);
                deleteTree(tmp);
            }
            xp3.delete();
            new File(tmp.getAbsolutePath() + ".status").delete();
            new File(tmp.getAbsolutePath() + ".progress").delete();
            markConfirmed();
            final File ready = dataDir;
            runOnUiThread(() -> {
                if (dataReady(ready)) startEngine(ready);
                else fail("解包完成但数据不可用");
            });
        } finally {
            DataExtractService.stop(this);
        }
    }

    private static String readText(File f) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[4096];
            int n = in.read(buf);
            return n > 0 ? new String(buf, 0, n, StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return null;
        }
    }

    private static void sleepQuietly(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException ignored) {}
    }

    private static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) deleteTree(c);
            }
        }
        f.delete();
    }

    private void fail(String message) {
        Log.e(TAG, message);
        runOnUiThread(() -> {
            setMessage(message);
            setProgress("", 0);
        });
    }

    // ---- JNI ----------------------------------------------------------------
    private static native boolean nativeExtractXp3Start(String xp3Path, String outDir);

    static {
        // "SDL2" first (its symbols are needed by the engine), then the
        // engine library itself - the native methods of this class live in
        // libmain.so. Best-effort: a library problem must never crash the
        // bootstrap page itself.
        try {
            System.loadLibrary("SDL2");
        } catch (Throwable ignored) {
            Log.e(TAG, "SDL2 library missing", ignored);
        }
        try {
            System.loadLibrary("main");
        } catch (Throwable ignored) {
            Log.e(TAG, "main library missing", ignored);
        }
    }
}
