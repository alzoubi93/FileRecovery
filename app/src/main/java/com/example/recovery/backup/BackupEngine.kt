package com.example.recovery.backup

import android.Manifest
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.core.content.ContextCompat
import java.io.File

/**
 * منطق الحماية الاستباقية:
 * 1) ينسخ الصور والفيديوهات الجديدة إلى مجلد خاص بالتطبيق.
 * 2) إذا اختفى ملف منسوخ من المعرض يُعلَّم "محذوفاً" ويبقى محفوظاً لمدة محددة.
 * 3) يطبّق سقفاً للمساحة: يُخلي أولاً النسخ التي ما زالت أصولها موجودة، ولا يمسّ المحذوفات إلا بعد انتهاء مدة الاحتفاظ.
 */
object BackupEngine {
    const val MAX_FILE = 200L * 1024 * 1024           // أكبر ملف يُنسخ
    const val CAP = 2L * 1024 * 1024 * 1024           // سقف المساحة الكلي
    const val MIN_FREE = 500L * 1024 * 1024           // لا ننسخ إن قلّت المساحة الحرة عن هذا
    const val FIRST_RUN_DAYS = 30L                    // في أول تشغيل ننسخ ملفات آخر 30 يوماً فقط
    const val DELETED_RETENTION_DAYS = 60L            // مدة بقاء المحذوفات

    fun rootDir(ctx: Context): File =
        File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "protected").apply { mkdirs() }

    private class Media(
        val key: String, val uri: Uri, val name: String, val relPath: String,
        val mime: String, val size: Long, val dateAdded: Long,
    )

    private fun hasAccess(ctx: Context): Boolean {
        if (Environment.isExternalStorageManager()) return true
        val perm = if (Build.VERSION.SDK_INT >= 33) Manifest.permission.READ_MEDIA_IMAGES
        else Manifest.permission.READ_EXTERNAL_STORAGE
        return ContextCompat.checkSelfPermission(ctx, perm) == PackageManager.PERMISSION_GRANTED
    }

    private fun queryAll(ctx: Context): List<Media> {
        val out = ArrayList<Media>()
        val sources = listOf(
            "i" to MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
            "v" to MediaStore.Video.Media.EXTERNAL_CONTENT_URI,
        )
        val proj = arrayOf(
            MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME,
            MediaStore.MediaColumns.RELATIVE_PATH, MediaStore.MediaColumns.MIME_TYPE,
            MediaStore.MediaColumns.SIZE, MediaStore.MediaColumns.DATE_ADDED,
        )
        for ((prefix, base) in sources) {
            ctx.contentResolver.query(base, proj, null, null, "${MediaStore.MediaColumns.DATE_ADDED} DESC")?.use { c ->
                val iId = c.getColumnIndexOrThrow(MediaStore.MediaColumns._ID)
                val iName = c.getColumnIndexOrThrow(MediaStore.MediaColumns.DISPLAY_NAME)
                val iRel = c.getColumnIndexOrThrow(MediaStore.MediaColumns.RELATIVE_PATH)
                val iMime = c.getColumnIndexOrThrow(MediaStore.MediaColumns.MIME_TYPE)
                val iSize = c.getColumnIndexOrThrow(MediaStore.MediaColumns.SIZE)
                val iDate = c.getColumnIndexOrThrow(MediaStore.MediaColumns.DATE_ADDED)
                while (c.moveToNext()) {
                    val id = c.getLong(iId)
                    out.add(
                        Media(
                            key = "$prefix$id", uri = ContentUris.withAppendedId(base, id),
                            name = c.getString(iName) ?: "file_$id", relPath = c.getString(iRel) ?: "",
                            mime = c.getString(iMime) ?: "", size = c.getLong(iSize), dateAdded = c.getLong(iDate),
                        )
                    )
                }
            }
        }
        return out
    }

    /** تأكيد إضافي قبل اعتبار الملف محذوفاً: هل ما زال يُفتح فعلاً؟ (يحمي من وحدة تخزين غير مركّبة مثلاً) */
    private fun stillExists(ctx: Context, key: String): Boolean {
        val base = if (key.startsWith("i")) MediaStore.Images.Media.EXTERNAL_CONTENT_URI
        else MediaStore.Video.Media.EXTERNAL_CONTENT_URI
        val id = key.substring(1).toLongOrNull() ?: return false
        return try {
            ctx.contentResolver.openAssetFileDescriptor(ContentUris.withAppendedId(base, id), "r")
                ?.use { true } ?: false
        } catch (e: Exception) {
            false
        }
    }

    private fun remove(root: File, index: BackupIndex, e: BackupEntry) {
        File(root, e.key).deleteRecursively()
        index.entries.remove(e.key)
    }

    /** يعيد (عدد الملفات المنسوخة، عدد الملفات التي اكتُشف حذفها). */
    fun run(ctx: Context): Pair<Int, Int> {
        if (!hasAccess(ctx)) return 0 to 0
        val root = rootDir(ctx)
        val index = BackupIndex(root).apply { load() }
        val now = System.currentTimeMillis()
        val current = queryAll(ctx)
        // قائمة فارغة مع فهرس غير فارغ تعني غالباً فقدان الوصول لا حذف كل شيء: لا نعلّم شيئاً محذوفاً
        if (current.isEmpty() && index.entries.isNotEmpty()) return 0 to 0

        val keys = current.mapTo(HashSet()) { it.key }
        var deleted = 0
        for ((k, e) in index.entries.toList()) {
            if (k !in keys) {
                if (e.deletedAt == 0L && !stillExists(ctx, k)) {
                    index.entries[k] = e.copy(deletedAt = now)
                    deleted++
                }
            } else if (e.deletedAt != 0L) {
                index.entries[k] = e.copy(deletedAt = 0)  // عاد للظهور في المعرض
            }
        }

        var copied = 0
        val cutoff = if (index.lastSync == 0L) now / 1000 - FIRST_RUN_DAYS * 86_400L else index.lastSync - 3600L
        if (root.usableSpace > MIN_FREE) {
            for (m in current) {
                if (m.key in index.entries || m.dateAdded < cutoff || m.size <= 0 || m.size > MAX_FILE) continue
                val dir = File(root, m.key).apply { mkdirs() }
                val safe = m.name.replace(Regex("[\\\\/:*?\"<>|]"), "_")
                val dest = File(dir, safe)
                val ok = try {
                    ctx.contentResolver.openInputStream(m.uri)?.use { input ->
                        dest.outputStream().use { out -> input.copyTo(out) }
                        true
                    } ?: false
                } catch (e: Exception) {
                    false
                }
                if (ok && dest.length() > 0) {
                    index.entries[m.key] = BackupEntry(
                        m.key, safe, m.relPath, m.mime, dest.length(), m.dateAdded, 0L, "${m.key}/$safe",
                    )
                    copied++
                    if (copied % 20 == 0) index.save()  // نحفظ الفهرس دورياً كي لا يضيع عند انقطاع العمل
                } else {
                    dir.deleteRecursively()
                }
            }
        }
        index.lastSync = now / 1000

        // سياسة الاحتفاظ
        val retentionMs = DELETED_RETENTION_DAYS * 86_400_000L
        index.entries.values.filter { it.deletedAt != 0L && now - it.deletedAt > retentionMs }
            .forEach { remove(root, index, it) }
        var total = index.entries.values.sumOf { it.size }
        if (total > CAP) {
            // الأولوية للإخلاء: النسخ التي ما زال أصلها موجوداً (الأقدم أولاً)، ثم المحذوفات الأقدم
            val victims = index.entries.values.sortedWith(
                compareBy<BackupEntry>({ it.deletedAt != 0L }, { if (it.deletedAt != 0L) it.deletedAt else it.dateAdded * 1000 })
            )
            for (v in victims) {
                if (total <= CAP) break
                remove(root, index, v)
                total -= v.size
            }
        }
        index.save()
        return copied to deleted
    }
}
