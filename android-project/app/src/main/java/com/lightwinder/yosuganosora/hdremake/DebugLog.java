package com.lightwinder.yosuganosora.hdremake;

import android.content.Context;
import android.os.Environment;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * File-based crash/log capture for devices without adb access (e.g. the
 * 卓易通 Android container on HarmonyOS). Writes Download/YosugaSoraHD/
 * crash.log when the public folder is writable, otherwise falls back to
 * the app-external files dir (Android/data/&lt;pkg&gt;/files/crash.log).
 * A process-wide uncaught-exception handler records fatal crashes with a
 * full stack trace before the process dies.
 */
public final class DebugLog {
    private static Context sContext;
    private static File sLogFile;

    private DebugLog() {}

    public static synchronized void init(Context context) {
        if (sContext != null) return;
        sContext = context.getApplicationContext();
        File dir = new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS), "YosugaSoraHD");
        try {
            if (!dir.exists() && !dir.mkdirs()) throw new IOException("mkdirs failed");
            sLogFile = new File(dir, "crash.log");
            if (!sLogFile.exists() && !sLogFile.createNewFile()) {
                throw new IOException("create failed");
            }
        } catch (Exception e) {
            File ext = sContext.getExternalFilesDir(null);
            sLogFile = ext != null ? new File(ext, "crash.log") : null;
        }
        log("debug log initialized; file=" + (sLogFile != null ? sLogFile.getAbsolutePath() : "null"));
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            try {
                logThrowable(throwable);
            } catch (Throwable ignored) {}
            android.os.Process.killProcess(android.os.Process.myPid());
        });
    }

    public static synchronized void log(String message) {
        try {
            if (sLogFile == null) return;
            String line = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US)
                    .format(new Date()) + " " + message + "\n";
            try (FileWriter fw = new FileWriter(sLogFile, true)) {
                fw.write(line);
                fw.flush();
            }
        } catch (Throwable ignored) {}
    }

    public static synchronized void log(String message, Throwable throwable) {
        StringWriter sw = new StringWriter();
        PrintWriter pw = new PrintWriter(sw);
        throwable.printStackTrace(pw);
        pw.flush();
        log(message + "\n" + sw);
    }

    public static synchronized void logThrowable(Throwable throwable) {
        log("FATAL", throwable);
    }
}
