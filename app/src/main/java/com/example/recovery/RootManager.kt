package com.example.recovery

data class PartitionInfo(val device: String, val mount: String, val fs: String, val encrypted: Boolean)

object RootManager {

    private fun su(cmd: String): Pair<Int, String> = try {
        val p = ProcessBuilder("su", "-c", cmd).redirectErrorStream(true).start()
        val out = p.inputStream.bufferedReader().readText()
        p.waitFor() to out
    } catch (e: Exception) {
        -1 to ""
    }

    fun isRootAvailable(): Boolean {
        val (code, out) = su("id")
        return code == 0 && out.contains("uid=0")
    }

    /** true إذا كان الجهاز بتشفير FBE، وفي هذه الحالة المسح العميق لقسم /data لن يعطي صوراً سليمة. */
    fun isFileBasedEncrypted(): Boolean =
        su("getprop ro.crypto.type").second.trim() == "file"

    /**
     * الأقسام المركّبة التي تستحق الفحص. نستبعد أقسام النظام (system/vendor...) لأنها للقراءة فقط،
     * ونعلّم أقسام البيانات المشفّرة (FBE) لأن مسحها الخام لا يعطي إلا بيانات مشفّرة.
     */
    fun listPartitions(): List<PartitionInfo> {
        val fbe = isFileBasedEncrypted()
        val skipFs = setOf("erofs", "squashfs", "overlay", "tmpfs")
        return su("cat /proc/mounts").second.lineSequence()
            .map { it.split(" ") }
            .filter { it.size > 2 && it[0].startsWith("/dev/block/") && it[2] !in skipFs }
            .filter { it[1].startsWith("/data") || it[1].startsWith("/storage") || it[1].startsWith("/mnt/") }
            .map { f ->
                val onData = f[1] == "/data" || f[1].startsWith("/data/") || f[1].startsWith("/mnt/user")
                PartitionInfo(device = f[0], mount = f[1], fs = f[2], encrypted = fbe && onData)
            }
            .distinctBy { it.device }
            .toList()
    }

    /** ملفات السلة والمصغّرات التي لا يراها التطبيق العادي (مثل .trashed-* في أندرويد 11+). */
    fun findTrashedFiles(): List<String> =
        su("find /data/media/0 /storage -type f \\( -name '.trashed-*' -o -path '*/.thumbnails/*' -o -path '*/.trash/*' \\) 2>/dev/null")
            .second.lines().filter { it.isNotBlank() }

    /** نسخ ملف إلى مجلد الاستعادة بصلاحية root ثم منح التطبيق ملكيته ليتمكن من قراءته. */
    fun copyOut(src: String, destDir: String, appUid: Int): Boolean {
        if (src.contains("'") || destDir.contains("'")) return false
        return su("cp '$src' '$destDir'/ && chown -R $appUid:$appUid '$destDir' && chmod -R u+rw '$destDir'").first == 0
    }
}
