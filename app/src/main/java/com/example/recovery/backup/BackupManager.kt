package com.example.recovery.backup

import android.content.Context
import androidx.work.Constraints
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import java.io.File
import java.util.concurrent.TimeUnit

object BackupManager {
    private const val WORK_NAME = "protect_media"

    private fun prefs(ctx: Context) = ctx.getSharedPreferences("protection", Context.MODE_PRIVATE)

    fun isEnabled(ctx: Context): Boolean = prefs(ctx).getBoolean("enabled", false)

    fun setEnabled(ctx: Context, on: Boolean) {
        prefs(ctx).edit().putBoolean("enabled", on).apply()
        val wm = WorkManager.getInstance(ctx)
        if (on) {
            val constraints = Constraints.Builder()
                .setRequiresBatteryNotLow(true)
                .setRequiresStorageNotLow(true)
                .build()
            val periodic = PeriodicWorkRequestBuilder<BackupWorker>(6, TimeUnit.HOURS)
                .setConstraints(constraints).build()
            wm.enqueueUniquePeriodicWork(WORK_NAME, ExistingPeriodicWorkPolicy.UPDATE, periodic)
            wm.enqueue(OneTimeWorkRequestBuilder<BackupWorker>().build())  // مزامنة فورية
        } else {
            wm.cancelUniqueWork(WORK_NAME)  // نوقف النسخ ونُبقي ما نُسخ سابقاً
        }
    }

    private fun loadIndex(ctx: Context): BackupIndex =
        BackupIndex(BackupEngine.rootDir(ctx)).apply { load() }

    fun stats(ctx: Context): ProtectionStats {
        val all = loadIndex(ctx).entries.values
        return ProtectionStats(
            protectedCount = all.count { it.deletedAt == 0L },
            deletedCount = all.count { it.deletedAt != 0L },
            bytes = all.sumOf { it.size },
        )
    }

    /** الملفات المنسوخة التي اختفت من المعرض، الأحدث حذفاً أولاً. */
    fun deletedFiles(ctx: Context): List<File> {
        val root = BackupEngine.rootDir(ctx)
        return loadIndex(ctx).entries.values
            .filter { it.deletedAt != 0L }
            .sortedByDescending { it.deletedAt }
            .map { File(root, it.file) }
            .filter { it.exists() }
    }
}
