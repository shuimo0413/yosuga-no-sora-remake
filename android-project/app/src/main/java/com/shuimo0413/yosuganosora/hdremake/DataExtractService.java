package com.shuimo0413.yosuganosora.hdremake;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/**
 * Foreground service that keeps the app alive (and exempt from background
 * reclamation) while game data is downloaded, unzipped or extracted.
 */
public class DataExtractService extends Service {
    private static final String CHANNEL_ID = "data_extract";
    private static final int NOTIFICATION_ID = 1;

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "游戏数据处理", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("下载 / 导入 / 解包游戏数据时保持运行");
            nm.createNotificationChannel(channel);
        }
        Intent notifyIntent = new Intent(this, BootstrapActivity.class);
        PendingIntent pending = PendingIntent.getActivity(this, 0, notifyIntent,
                PendingIntent.FLAG_IMMUTABLE);
        Notification notification = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                    .setContentTitle("正在处理游戏数据")
                    .setContentText("下载 / 导入 / 解包进行中…")
                    .setSmallIcon(android.R.drawable.stat_sys_download)
                    .setContentIntent(pending)
                    .build()
                : new Notification.Builder(this)
                    .setContentTitle("正在处理游戏数据")
                    .setContentText("下载 / 导入 / 解包进行中…")
                    .setSmallIcon(android.R.drawable.stat_sys_download)
                    .setContentIntent(pending)
                    .build();
        startForeground(NOTIFICATION_ID, notification);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    static void start(Context context) {
        Intent intent = new Intent(context, DataExtractService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(intent);
        } else {
            context.startService(intent);
        }
    }

    static void stop(Context context) {
        context.stopService(new Intent(context, DataExtractService.class));
    }
}
