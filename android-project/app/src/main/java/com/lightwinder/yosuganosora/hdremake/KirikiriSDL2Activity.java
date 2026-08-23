package com.lightwinder.yosuganosora.hdremake;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetFileDescriptor;
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

    private static native void nativeOnMovieFinished();
    private static native void nativeOnMovieError(String message);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mLayout == null) return;

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

        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT);
        mLayout.addView(movieView, params);

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

    // App-external save folder: Android/data/<pkg>/savedata.  NOT the default
    // <pkg>/files/DownloadSavedata, which was a wrong concatenation that made
    // saves unreadable across launches on Android 10.
    private File getAppExternalSaveDir() {
        // Do NOT call getExternalFilesDir(null): Android implicitly creates the
        // Android/data/<pkg>/files directory whenever getExternalFilesDir() is
        // invoked, leaving an empty <pkg>/files folder on every launch.  Derive
        // the app-external package dir from external storage instead so saves
        // live at Android/data/<pkg>/savedata without the spurious files folder.
        File external = Environment.getExternalStorageDirectory();
        if (external == null) return null;
        File pkgDir = new File(external, "Android/data/" + getPackageName());
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
            movieHasBounds = width > 0 && height > 0 && logicalWidth > 0 && logicalHeight > 0;
            if (movieView != null && movieHasBounds) {
                // Scale logical game coordinates to the actual view size.
                int viewW = mLayout.getWidth();
                int viewH = mLayout.getHeight();
                if (viewW > 0 && viewH > 0) {
                    float sx = (float) viewW / logicalWidth;
                    float sy = (float) viewH / logicalHeight;
                    float px = left * sx;
                    float py = top * sy;
                    float pw = width * sx;
                    float ph = height * sy;
                    movieView.setX(px);
                    movieView.setY(py);
                    ViewGroup.LayoutParams lp = movieView.getLayoutParams();
                    if (lp instanceof RelativeLayout.LayoutParams) {
                        RelativeLayout.LayoutParams rlp =
                            (RelativeLayout.LayoutParams) lp;
                        rlp.width = (int) pw;
                        rlp.height = (int) ph;
                        rlp.leftMargin = 0;
                        rlp.topMargin = 0;
                    }
                    movieView.setLayoutParams(lp);
                    movieView.requestLayout();
                    fitMovieView();
                }
            } else if (movieView != null) {
                // No bounds: fullscreen fallback (OP/ED).
                movieView.setX(0);
                movieView.setY(0);
                ViewGroup.LayoutParams lp = movieView.getLayoutParams();
                if (lp instanceof RelativeLayout.LayoutParams) {
                    RelativeLayout.LayoutParams rlp =
                        (RelativeLayout.LayoutParams) lp;
                    rlp.width = ViewGroup.LayoutParams.MATCH_PARENT;
                    rlp.height = ViewGroup.LayoutParams.MATCH_PARENT;
                    rlp.leftMargin = 0;
                    rlp.topMargin = 0;
                }
                movieView.setLayoutParams(lp);
                movieView.requestLayout();
                fitMovieView();
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
        super.onDestroy();
    }
}
