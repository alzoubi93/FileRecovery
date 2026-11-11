package com.example.recovery.backup

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class BackupEntry(
    val key: String,        // "i<ID>" للصور و "v<ID>" للفيديو (معرّف MediaStore)
    val name: String,
    val relPath: String,    // المسار النسبي الأصلي، مثل Pictures/Camera/
    val mime: String,
    val size: Long,
    val dateAdded: Long,    // بالثواني
    val deletedAt: Long,    // بالميلي ثانية؛ 0 يعني أن الملف ما زال موجوداً في المعرض
    val file: String,       // مسار الملف المنسوخ نسبةً لمجلد الحماية
)

data class ProtectionStats(val protectedCount: Int = 0, val deletedCount: Int = 0, val bytes: Long = 0)

/** فهرس النسخ الاحتياطية، محفوظ كـ JSON بجانب الملفات المنسوخة. */
class BackupIndex(private val root: File) {
    private val indexFile = File(root, "index.json")
    val entries = LinkedHashMap<String, BackupEntry>()
    var lastSync: Long = 0  // بالثواني

    fun load() {
        entries.clear()
        lastSync = 0
        if (!indexFile.exists()) return
        try {
            val obj = JSONObject(indexFile.readText())
            lastSync = obj.optLong("lastSync", 0)
            val arr = obj.optJSONArray("entries") ?: JSONArray()
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val e = BackupEntry(
                    key = o.getString("key"), name = o.getString("name"),
                    relPath = o.optString("relPath"), mime = o.optString("mime"),
                    size = o.optLong("size"), dateAdded = o.optLong("dateAdded"),
                    deletedAt = o.optLong("deletedAt"), file = o.getString("file"),
                )
                entries[e.key] = e
            }
        } catch (e: Exception) {
            entries.clear()
            lastSync = 0
        }
    }

    fun save() {
        val arr = JSONArray()
        for (e in entries.values) {
            arr.put(
                JSONObject().put("key", e.key).put("name", e.name).put("relPath", e.relPath)
                    .put("mime", e.mime).put("size", e.size).put("dateAdded", e.dateAdded)
                    .put("deletedAt", e.deletedAt).put("file", e.file)
            )
        }
        val obj = JSONObject().put("lastSync", lastSync).put("entries", arr)
        val tmp = File(root, "index.json.tmp")
        tmp.writeText(obj.toString())
        if (!tmp.renameTo(indexFile)) {  // الكتابة عبر ملف مؤقت لتفادي فهرس نصف مكتوب
            indexFile.delete()
            tmp.renameTo(indexFile)
        }
    }
}
