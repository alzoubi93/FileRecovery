package com.example.recovery

import android.app.Application
import android.content.ContentValues
import android.provider.MediaStore
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.example.recovery.backup.BackupManager
import com.example.recovery.backup.ProtectionStats
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.io.File

enum class Screen { Home, Scanning, Results }

data class UiState(
    val screen: Screen = Screen.Home,
    val rootAvailable: Boolean = false,
    val encrypted: Boolean = false,
    val partitions: List<PartitionInfo> = emptyList(),
    val label: String = "",
    val bytes: Long = 0,
    val found: Int = 0,
    val results: List<File> = emptyList(),
    val selected: Set<String> = emptySet(),
    val toast: String? = null,
    val protectionOn: Boolean = false,
    val stats: ProtectionStats = ProtectionStats(),
)

class RecoveryViewModel(app: Application) : AndroidViewModel(app) {

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private val outDir = File(app.getExternalFilesDir(null), "recovered")
    private val recoverableExt = setOf("jpg", "jpeg", "png", "webp", "gif", "heic", "mp4", "mov", "3gp", "m4a", "pdf", "docx", "xlsx", "pptx", "zip", "apk",
        "heif", "bmp", "dng", "mkv", "webm", "avi")
    @Volatile private var cancelled = false

    init {
        refreshProtection()
        viewModelScope.launch(Dispatchers.IO) {
            val root = RootManager.isRootAvailable()
            _state.update {
                it.copy(
                    rootAvailable = root,
                    encrypted = root && RootManager.isFileBasedEncrypted(),
                    partitions = if (root) RootManager.listPartitions() else emptyList(),
                )
            }
        }
    }

    fun startQuickScan() = runScan("المسح السريع") {
        val root = _state.value.rootAvailable
        val uid = android.os.Process.myUid()
        val paths = if (root) RootManager.findTrashedFiles() else QuickScanner.find()
        var n = 0
        for (p in paths) {
            if (cancelled) break
            val src = File(p)
            val copied = if (root) RootManager.copyOut(p, outDir.absolutePath, uid)
            else runCatching { src.copyTo(File(outDir, src.name), true) }.isSuccess
            if (copied) {
                normalize(File(outDir, src.name))
                _state.update { it.copy(found = ++n) }
            }
        }
    }

    fun startDeepScan(device: String) = runScan("المسح العميق") {
        val poll = viewModelScope.launch {
            while (true) {
                _state.update { it.copy(bytes = NativeCarver.progressBytes(), found = NativeCarver.progressCount()) }
                delay(300)
            }
        }
        val code = NativeCarver.carveDevice(device, outDir)
        poll.cancel()
        if (code < 0) _state.update { it.copy(toast = "تعذّرت قراءة القسم. تأكد من صلاحية root.") }
    }

    fun cancelScan() {
        cancelled = true
        NativeCarver.cancel()
    }

    private fun runScan(label: String, block: suspend () -> Unit) {
        cancelled = false
        viewModelScope.launch(Dispatchers.IO) {
            outDir.deleteRecursively(); outDir.mkdirs()
            _state.update {
                it.copy(screen = Screen.Scanning, label = label, bytes = 0, found = 0,
                    results = emptyList(), selected = emptySet())
            }
            try { block() } catch (e: Exception) {
                _state.update { it.copy(toast = "تعذّر إكمال المسح") }
            }
            val files = outDir.listFiles()
                ?.filter { it.extension.lowercase() in recoverableExt }
                ?.sortedByDescending { it.lastModified() }
                ?: emptyList()
            _state.update { it.copy(screen = Screen.Results, results = files) }
        }
    }

    /** يزيل بادئة .trashed- ويضيف امتداد jpg للمصغّرات التي بلا امتداد. */
    private fun normalize(f: File) {
        if (!f.exists()) return
        var name = f.name.replace(Regex("^\\.trashed-\\d+-"), "")
        if (name.substringAfterLast('.', "").lowercase() !in recoverableExt) name += ".jpg"
        if (name != f.name) f.renameTo(File(f.parentFile, name))
    }

    fun toggle(path: String) = _state.update {
        it.copy(selected = if (path in it.selected) it.selected - path else it.selected + path)
    }

    fun selectAll() = _state.update { it.copy(selected = it.results.map { f -> f.absolutePath }.toSet()) }

    fun backHome() {
        _state.update { it.copy(screen = Screen.Home) }
        refreshProtection()
    }

    /** يحدّث حالة الحماية الاستباقية وإحصاءاتها (يُستدعى عند الفتح والرجوع للرئيسية). */
    fun refreshProtection() {
        viewModelScope.launch(Dispatchers.IO) {
            val app = getApplication<Application>()
            val on = BackupManager.isEnabled(app)
            val st = BackupManager.stats(app)
            _state.update { it.copy(protectionOn = on, stats = st) }
        }
    }

    fun setProtection(on: Boolean) {
        BackupManager.setEnabled(getApplication(), on)
        _state.update { it.copy(protectionOn = on) }
    }

    fun showToast(msg: String) = _state.update { it.copy(toast = msg) }

    /** يعرض الملفات المحذوفة التي نسخناها مسبقاً، لتُستعاد بنفس شاشة النتائج. */
    fun showProtected() {
        viewModelScope.launch(Dispatchers.IO) {
            val files = BackupManager.deletedFiles(getApplication())
            _state.update { it.copy(screen = Screen.Results, results = files, selected = emptySet()) }
        }
    }

    fun consumeToast() = _state.update { it.copy(toast = null) }

    private class Target(val uri: android.net.Uri, val mime: String, val dir: String)

    private fun targetFor(ext: String): Target = when (ext.lowercase()) {
        "png" -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/png", "Pictures/Recovered")
        "webp" -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/webp", "Pictures/Recovered")
        "heic" -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/heic", "Pictures/Recovered")
        "mp4" -> Target(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/mp4", "Movies/Recovered")
        "3gp" -> Target(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/3gpp", "Movies/Recovered")
        "mov" -> Target(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/quicktime", "Movies/Recovered")
        "m4a" -> Target(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, "audio/mp4", "Music/Recovered")
        "heif" -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/heif", "Pictures/Recovered")
        "bmp" -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/bmp", "Pictures/Recovered")
        "dng" -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/x-adobe-dng", "Pictures/Recovered")
        "mkv" -> Target(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/x-matroska", "Movies/Recovered")
        "webm" -> Target(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/webm", "Movies/Recovered")
        "avi" -> Target(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/x-msvideo", "Movies/Recovered")
        "gif" -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/gif", "Pictures/Recovered")
        "pdf" -> Target(MediaStore.Downloads.EXTERNAL_CONTENT_URI, "application/pdf", "Download/Recovered")
        "docx" -> Target(MediaStore.Downloads.EXTERNAL_CONTENT_URI, "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "Download/Recovered")
        "xlsx" -> Target(MediaStore.Downloads.EXTERNAL_CONTENT_URI, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "Download/Recovered")
        "pptx" -> Target(MediaStore.Downloads.EXTERNAL_CONTENT_URI, "application/vnd.openxmlformats-officedocument.presentationml.presentation", "Download/Recovered")
        "zip" -> Target(MediaStore.Downloads.EXTERNAL_CONTENT_URI, "application/zip", "Download/Recovered")
        "apk" -> Target(MediaStore.Downloads.EXTERNAL_CONTENT_URI, "application/vnd.android.package-archive", "Download/Recovered")
        else -> Target(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/jpeg", "Pictures/Recovered")
    }

    /** حفظ الملفات المحددة في المجلد المناسب لنوعها (صور، فيديو، صوت، PDF). */
    fun saveSelected() {
        val s = _state.value
        val files = s.results.filter { it.absolutePath in s.selected }
        viewModelScope.launch(Dispatchers.IO) {
            val resolver = getApplication<Application>().contentResolver
            var saved = 0
            for (f in files) {
                val t = targetFor(f.extension)
                val values = ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, f.name.trimStart('.'))
                    put(MediaStore.MediaColumns.MIME_TYPE, t.mime)
                    put(MediaStore.MediaColumns.RELATIVE_PATH, t.dir)
                }
                val uri = resolver.insert(t.uri, values) ?: continue
                resolver.openOutputStream(uri)?.use { out -> f.inputStream().use { it.copyTo(out) } }
                saved++
            }
            _state.update { it.copy(toast = "تم حفظ $saved ملف في مجلد Recovered", selected = emptySet()) }
        }
    }
}
