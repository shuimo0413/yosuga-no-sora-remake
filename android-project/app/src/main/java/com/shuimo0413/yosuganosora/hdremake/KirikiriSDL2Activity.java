package com.shuimo0413.yosuganosora.hdremake;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetFileDescriptor;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.IOException; 

public class KirikiriSDL2Activity extends SDLActivity {
    private static final String TAG = "KirikiriSDL2";
    private static final String ASSET_PREFIX = "asset:///";

    private static final int STORAGE_PERMISSION_REQUEST = 9001;
    private static final String SAVE_SUBDIR = "YosugaSoraHD" + File.separator + "savedata";
    private static final String NO_MEDIA = ".nomedia";
    /** Game art is 16:9; the SDL surface is letterboxed to this aspect (OHOS parity). */
    private static final int GAME_ASPECT_W = 16;
    private static final int GAME_ASPECT_H = 9;
    // Cached public save directory absolute path (UTF-8), shared with native.
    private static String sPublicSaveDir = null;

    private TextureView movieView;
    private MediaPlayer moviePlayer;
    private String pendingMoviePath;
    private boolean moviePrepared;
    private boolean playMovieWhenPrepared;
    private float movieVolume = 1.0f;
    // Movie display rectangle in the SDL surface's coordinate space; the
    // engine reports it through setMovieBounds().  A zero-size rectangle
    // means "fill the whole window" (default fullscreen movies).
    private int movieLeft, movieTop, movieWidth, movieHeight;
    private boolean movieHasBounds;
    private int screenWidthPx, screenHeightPx;
    // Last parent size passed to applyGameFrameLayout (skip redundant relayout).
    private int lastLayoutWidth = -1;
    private int lastLayoutHeight = -1;
    // Centered 16:9 frame origin inside mLayout (explicit margins, not setX/Y).
    private int frameOffsetX = 0;
    private int frameOffsetY = 0;
    private int frameWidthPx = 0;
    private int frameHeightPx = 0;
    // Logical movie bounds from native; replayed after surface relayout.
    private int movieLogicalLeft, movieLogicalTop, movieLogicalWidth, movieLogicalHeight;
    private int movieLogicalWindowW, movieLogicalWindowH;

    private static native void nativeOnMovieFinished();
    private static native void nativeOnMovieError(String message);
    // External data flow: the bootstrap activity reports the extracted data
    // directory; the engine resolves ./data/* against it (see
    // AndroidDataBridge.cpp / StorageImpl.cpp). Empty string keeps the
    // bundled APK assets as the data source.
    private static native void nativeSetDataDir(String dataDir);
    private static native void nativeDetachExtractThread();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String dataDir = getIntent().getStringExtra("dataDir");
        if (dataDir != null) {
            nativeSetDataDir(dataDir);
        }
        if (mLayout == null) return;

        mLayout.setBackgroundColor(Color.BLACK);

        movieView = new TextureView(this);
        movieView.setOpaque(true);
        movieView.setVisibility(View.GONE);
        movieView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) {
                if (pendingMoviePath != null) prepareMovieOnUiThread(pendingMoviePath, texture);
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) {}

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
                releaseMovieOnUiThread(false);
                return true;
            }

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture texture) {}
        });

        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(0, 0);
        params.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
        params.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        mLayout.addView(movieView, params);

        mLayout.addOnLayoutChangeListener((v, left, top, right, bottom,
                oldLeft, oldTop, oldRight, oldBottom) -> {
            int w = right - left;
            int h = bottom - top;
            if (w > 0 && h > 0 && (w != lastLayoutWidth || h != lastLayoutHeight)) {
                applyGameFrameLayout();
            }
        });
        mLayout.post(this::applyGameFrameLayout);

        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        screenWidthPx = dm.widthPixels;
        screenHeightPx = dm.heightPixels;

        // Ask for the storage permission needed to write to public Downloads.
        // On Android 11+ that means MANAGE_EXTERNAL_STORAGE (opens system
        // settings); on older versions the WRITE/READ pair is requested.
        requestStoragePermissionIfNeeded();

        // Build the public save directory (and .nomedia marker) so saves live
        // in a user-reachable folder.  Old saves are NOT auto-migrated here;
        // a fresh install simply starts using the new location.
        getPublicSaveDataPath();
    }

    // ---- Storage permission + public save directory -----------------------
    private void requestStoragePermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Android 11+: scoped storage requires MANAGE_EXTERNAL_STORAGE,
            // which can only be granted from system Settings.
            if (!Environment.isExternalStorageManager()) {
                try {
                    Intent intent = new Intent(
                        Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                    startActivity(intent);
                } catch (Exception ignored) {
                    try {
                        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
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

    // App-external save folder: Android/data/<pkg>/savedata (DIRECTLY under the
    // package dir, NOT under getExternalFilesDir's .../files second-level dir).
    private File getAppExternalSaveDir() {
        // getExternalFilesDir() is the RELIABLE way to locate the app-external
        // directory on every Android version (incl. scoped storage on 11+), but
        // it inherently creates Android/data/<pkg>/files. Take the parent <pkg>
        // dir so saves land in Android/data/<pkg>/savedata, then delete the
        // just-created empty <pkg>/files folder so no spurious files directory
        // is left behind.
        File ext = getExternalFilesDir(null);
        if (ext == null) return null;
        File pkgDir = ext.getParentFile();
        try {
            if (ext.exists() && ext.isDirectory()) {
                String[] children = ext.list();
                if (children != null && children.length == 0) ext.delete();
            }
        } catch (SecurityException e) { /* ignore */ }
        return pkgDir != null ? new File(pkgDir, "savedata") : null;
    }

    private boolean hasPublicStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true; // granted at install time on <= API 22
    }

    /**
     * Returns the public Downloads save folder path, creating it (plus a
     * .nomedia marker so the media scanner never publishes the save thumbnails
     * into the system gallery) if needed.  Returns null when public access is
     * unavailable.  Called from native through JNI; keep it public and
     * side-effect safe.
     */
    public String getPublicSaveDataPath() {
        if (sPublicSaveDir != null) return sPublicSaveDir;
        if (!hasPublicStorageAccess()) return null;

        // Prefer the real public Downloads folder on every supported Android
        // so saves are user-reachable.  On Android 11+ MANAGE_EXTERNAL_STORAGE
        // makes it writable; on Android 10 the manifest's requestLegacy
        // ExternalStorage opts the app into legacy storage so the public
        // Download path is also writable.  If scoped storage still blocks it
        // (e.g. the legacy flag was ignored on some devices), fall back to the
        // app-external folder, which is always writable and still reachable
        // via the Files app / USB and persists across app un-installs.
        File saveDir = null;
        File publicDir = Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS);
        if (publicDir != null) saveDir = new File(publicDir, SAVE_SUBDIR);
        if (saveDir == null) {
                            saveDir = getAppExternalSaveDir();
        }
        if (saveDir == null) return null;

        try {
            if (!saveDir.exists() && !saveDir.mkdirs()) {
                // Public Downloads write was blocked; use the app-external folder.
                                saveDir = getAppExternalSaveDir();
                if (!saveDir.exists() && !saveDir.mkdirs()) return null;
            }
            File noMedia = new File(saveDir, NO_MEDIA);
            if (!noMedia.exists()) {
                if (!noMedia.createNewFile())
                    Log.w(TAG, "Could not create " + NO_MEDIA);
            }
            sPublicSaveDir = saveDir.getAbsolutePath();
        } catch (IOException | SecurityException e) {
            Log.e(TAG, "Failed to prepare public save directory", e);
            return null;
        }
        return sPublicSaveDir;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
            int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == STORAGE_PERMISSION_REQUEST
                && grantResults.length > 0
                && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            // Re-run public dir setup now that permission is granted.
            getPublicSaveDataPath();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // If the user returns from the system Settings screen (where they
        // granted MANAGE_EXTERNAL_STORAGE on Android 11+), try to set up the
        // public save directory now.  The native helper re-queries this
        // method, so an in-session grant takes effect without a restart.
        if (sPublicSaveDir == null && hasPublicStorageAccess())
            getPublicSaveDataPath();
    }

    /**
     * The game has a fixed 1920x1080 coordinate system. SDL normally promotes
     * resizable windows to FULL_USER orientation, which can recreate the
     * Surface in portrait when an Android device resumes. Keep both the Java
     * activity and SDL's later orientation request locked to landscape.
     */
    @Override
    public void setOrientationBis(int width, int height, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    /**
     * Sizes {@link #mSurface} and {@link #movieView} to a centered 16:9 rect
     * inside {@link #mLayout}. Uses explicit left/top margins (Bootstrap /
     * OHOS parity) instead of CENTER_IN_PARENT + setX/setY, which pulled the
     * intro movie flush-left on ultrawide screens.
     */
    private void applyGameFrameLayout() {
        if (mLayout == null || mSurface == null) return;

        int parentW = mLayout.getWidth();
        int parentH = mLayout.getHeight();
        if (parentW <= 0 || parentH <= 0) return;
        if (parentW == lastLayoutWidth && parentH == lastLayoutHeight) return;

        lastLayoutWidth = parentW;
        lastLayoutHeight = parentH;

        int[] frame = computeGameFrameSize(parentW, parentH);
        int frameW = frame[0];
        int frameH = frame[1];
        if (frameW <= 0 || frameH <= 0) return;

        int offsetX = (parentW - frameW) / 2;
        int offsetY = (parentH - frameH) / 2;
        frameOffsetX = offsetX;
        frameOffsetY = offsetY;
        frameWidthPx = frameW;
        frameHeightPx = frameH;

        RelativeLayout.LayoutParams surfaceLp =
            new RelativeLayout.LayoutParams(frameW, frameH);
        surfaceLp.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
        surfaceLp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        surfaceLp.leftMargin = offsetX;
        surfaceLp.topMargin = offsetY;
        surfaceLp.rightMargin = 0;
        surfaceLp.bottomMargin = 0;
        mSurface.setLayoutParams(surfaceLp);
        mSurface.setTranslationX(0f);
        mSurface.setTranslationY(0f);

        if (movieView != null) {
            RelativeLayout.LayoutParams movieLp =
                new RelativeLayout.LayoutParams(frameW, frameH);
            movieLp.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
            movieLp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
            movieLp.leftMargin = offsetX;
            movieLp.topMargin = offsetY;
            movieLp.rightMargin = 0;
            movieLp.bottomMargin = 0;
            movieView.setLayoutParams(movieLp);
            movieView.setTranslationX(0f);
            movieView.setTranslationY(0f);
        }

        mLayout.requestLayout();
        syncSurfaceHolderSize();
        replayMovieBoundsIfNeeded();
    }

    /** Integer contain-16:9 inside parentW x parentH (clamped, no overflow). */
    private static int[] computeGameFrameSize(int parentW, int parentH) {
        if (parentW <= 0 || parentH <= 0) {
            return new int[]{0, 0};
        }
        int frameW;
        int frameH;
        long parentWScaled = (long) parentW * GAME_ASPECT_H;
        long parentHScaled = (long) parentH * GAME_ASPECT_W;
        if (parentWScaled > parentHScaled) {
            frameH = parentH;
            frameW = (int) ((long) frameH * GAME_ASPECT_W / GAME_ASPECT_H);
            if (frameW > parentW) frameW = parentW;
        } else {
            frameW = parentW;
            frameH = (int) ((long) frameW * GAME_ASPECT_H / GAME_ASPECT_W);
            if (frameH > parentH) frameH = parentH;
        }
        return new int[]{frameW, frameH};
    }

    /**
     * Asks the SurfaceView holder to follow the view layout so the compositor
     * does not keep an old left-aligned surface after margin centering.
     */
    private void syncSurfaceHolderSize() {
        if (!(mSurface instanceof SurfaceView)) return;
        SurfaceView surfaceView = (SurfaceView) mSurface;
        surfaceView.post(() -> {
            if (surfaceView.getHolder() != null) {
                surfaceView.getHolder().setSizeFromLayout();
            }
        });
    }

    private void replayMovieBoundsIfNeeded() {
        if (movieView == null) return;
        if (movieHasBounds) {
            applyMovieBounds(movieLogicalLeft, movieLogicalTop,
                movieLogicalWidth, movieLogicalHeight,
                movieLogicalWindowW, movieLogicalWindowH);
        } else if (movieView.getVisibility() == View.VISIBLE || pendingMoviePath != null) {
            applyMovieBounds(0, 0, 0, 0, 0, 0);
        }
    }

    /**
     * Maps engine logical movie bounds onto the centered game surface rect
     * using the same margin origin as {@link #applyGameFrameLayout()}.
     */
    private void applyMovieBounds(int left, int top, int width, int height,
            int logicalWidth, int logicalHeight) {
        if (movieView == null || mSurface == null) return;

        int frameW = frameWidthPx > 0 ? frameWidthPx : mSurface.getWidth();
        int frameH = frameHeightPx > 0 ? frameHeightPx : mSurface.getHeight();
        if (frameW <= 0 || frameH <= 0) {
            movieView.post(() -> applyMovieBounds(left, top, width, height,
                logicalWidth, logicalHeight));
            return;
        }

        boolean hasBounds = width > 0 && height > 0 && logicalWidth > 0 && logicalHeight > 0;

        ViewGroup.LayoutParams lp = movieView.getLayoutParams();
        if (!(lp instanceof RelativeLayout.LayoutParams)) {
            lp = new RelativeLayout.LayoutParams(0, 0);
        }
        RelativeLayout.LayoutParams rlp = (RelativeLayout.LayoutParams) lp;
        rlp.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
        rlp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        rlp.removeRule(RelativeLayout.CENTER_IN_PARENT);
        rlp.removeRule(RelativeLayout.CENTER_HORIZONTAL);
        rlp.removeRule(RelativeLayout.CENTER_VERTICAL);
        rlp.rightMargin = 0;
        rlp.bottomMargin = 0;

        if (hasBounds) {
            float sx = (float) frameW / logicalWidth;
            float sy = (float) frameH / logicalHeight;
            int pw = (int) (width * sx);
            int ph = (int) (height * sy);
            rlp.width = pw;
            rlp.height = ph;
            rlp.leftMargin = frameOffsetX + (int) (left * sx);
            rlp.topMargin = frameOffsetY + (int) (top * sy);
        } else {
            rlp.width = frameW;
            rlp.height = frameH;
            rlp.leftMargin = frameOffsetX;
            rlp.topMargin = frameOffsetY;
        }
        movieView.setLayoutParams(rlp);
        movieView.setTranslationX(0f);
        movieView.setTranslationY(0f);
        movieView.requestLayout();
        fitMovieView();
    }

    /**
     * Places the movie view inside the engine's overlay rectangle.
     *
     * The engine reports the rectangle in game-space logical coordinates
     * (e.g. 1920x1080) together with the logical window size; this method
     * scales those onto the actual view pixels so the video lands exactly on
     * the intended overlay area instead of being stretched fullscreen.
     * Called from native through JNI.
     */
    public void setMovieBounds(final int left, final int top,
            final int width, final int height,
            final int logicalWidth, final int logicalHeight) {
        runOnUiThread(() -> {
            movieLeft = left;
            movieTop = top;
            movieWidth = width;
            movieHeight = height;
            movieLogicalLeft = left;
            movieLogicalTop = top;
            movieLogicalWidth = width;
            movieLogicalHeight = height;
            movieLogicalWindowW = logicalWidth;
            movieLogicalWindowH = logicalHeight;
            movieHasBounds = width > 0 && height > 0 && logicalWidth > 0 && logicalHeight > 0;
            if (movieView != null) {
                applyMovieBounds(left, top, width, height, logicalWidth, logicalHeight);
            }
        });
    }

    /**
     * Applies a letterbox transform to the movie TextureView so the video
     * keeps its native aspect ratio inside the current view rectangle
     * instead of being stretched.  Fullscreen movies simply fill the window.
     */
    private void fitMovieView() {
        if (movieView == null || moviePlayer == null) return;
        int vw = moviePlayer.getVideoWidth();
        int vh = moviePlayer.getVideoHeight();
        if (vw <= 0 || vh <= 0) return;

        int viewW = movieView.getWidth();
        int viewH = movieView.getHeight();
        if (viewW <= 0 || viewH <= 0) {
            // Layout not done yet; try again after the frame is measured.
            movieView.post(() -> fitMovieView());
            return;
        }

        float scale = Math.min((float) viewW / vw, (float) viewH / vh);
        float scaledW = vw * scale;
        float scaledH = vh * scale;
        float tx = (viewW - scaledW) / 2f;
        float ty = (viewH - scaledH) / 2f;

        android.graphics.Matrix matrix = new android.graphics.Matrix();
        matrix.setScale(scaledW / viewW, scaledH / viewH);
        matrix.postTranslate(tx, ty);
        movieView.setTransform(matrix);
    }

    public void openMovie(final String path) {
        runOnUiThread(() -> {
            releaseMovieOnUiThread(false);
            pendingMoviePath = path;
            playMovieWhenPrepared = false;
            moviePrepared = false;
            if (movieView == null) {
                reportMovieError("movie TextureView is unavailable");
                return;
            }
            movieView.setVisibility(View.VISIBLE);
            movieView.setKeepScreenOn(true);
            if (movieView.isAvailable()) {
                prepareMovieOnUiThread(path, movieView.getSurfaceTexture());
            }
        });
    }

    private void prepareMovieOnUiThread(String path, SurfaceTexture texture) {
        if (texture == null || path == null || !path.equals(pendingMoviePath) || moviePlayer != null) return;

        MediaPlayer player = new MediaPlayer();
        moviePlayer = player;
        Surface surface = new Surface(texture);
        AssetFileDescriptor asset = null;
        try {
            player.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                .build());
            player.setSurface(surface);
            player.setVolume(movieVolume, movieVolume);
            player.setOnPreparedListener(preparedPlayer -> {
                if (preparedPlayer != moviePlayer) return;
                moviePrepared = true;
                fitMovieView();
                if (playMovieWhenPrepared) preparedPlayer.start();
            });
            player.setOnCompletionListener(completedPlayer -> {
                if (completedPlayer != moviePlayer) return;
                releaseMovieOnUiThread(true);
                nativeOnMovieFinished();
            });
            player.setOnErrorListener((failedPlayer, what, extra) -> {
                if (failedPlayer == moviePlayer) {
                    reportMovieError("MediaPlayer error what=" + what + ", extra=" + extra);
                }
                return true;
            });

            if (path.startsWith(ASSET_PREFIX)) {
                String assetPath = path.substring(ASSET_PREFIX.length());
                asset = getAssets().openFd(assetPath);
                player.setDataSource(asset.getFileDescriptor(), asset.getStartOffset(), asset.getLength());
            } else {
                player.setDataSource(path);
            }
            player.prepareAsync();
        } catch (Exception error) {
            reportMovieError(error.toString());
        } finally {
            surface.release();
            if (asset != null) {
                try {
                    asset.close();
                } catch (Exception ignored) {}
            }
        }
    }

    public void playMovie() {
        runOnUiThread(() -> {
            playMovieWhenPrepared = true;
            if (moviePlayer != null && moviePrepared) moviePlayer.start();
        });
    }

    public void pauseMovie() {
        runOnUiThread(() -> {
            playMovieWhenPrepared = false;
            if (moviePlayer != null && moviePrepared && moviePlayer.isPlaying()) moviePlayer.pause();
        });
    }

    public void rewindMovie() {
        runOnUiThread(() -> {
            if (moviePlayer != null && moviePrepared) moviePlayer.seekTo(0);
        });
    }

    public void stopMovie() {
        runOnUiThread(() -> releaseMovieOnUiThread(false));
    }

    public void setMovieVolume(final float volume) {
        movieVolume = Math.max(0.0f, Math.min(1.0f, volume));
        runOnUiThread(() -> {
            if (moviePlayer != null) moviePlayer.setVolume(movieVolume, movieVolume);
        });
    }

    private void reportMovieError(String message) {
        Log.e(TAG, "Movie playback failed: " + message);
        releaseMovieOnUiThread(true);
        nativeOnMovieError(message);
    }

    private void releaseMovieOnUiThread(boolean keepCurrentSurface) {
        pendingMoviePath = null;
        playMovieWhenPrepared = false;
        moviePrepared = false;
        if (moviePlayer != null) {
            moviePlayer.setOnPreparedListener(null);
            moviePlayer.setOnCompletionListener(null);
            moviePlayer.setOnErrorListener(null);
            moviePlayer.reset();
            moviePlayer.release();
            moviePlayer = null;
        }
        if (!keepCurrentSurface && movieView != null) {
            movieView.setKeepScreenOn(false);
            movieView.setVisibility(View.GONE);
        } else if (movieView != null) {
            movieView.setKeepScreenOn(false);
            movieView.setVisibility(View.GONE);
        }
    }

    @Override
    protected void onDestroy() {
        releaseMovieOnUiThread(false);
        nativeDetachExtractThread();
        super.onDestroy();
    }
}
