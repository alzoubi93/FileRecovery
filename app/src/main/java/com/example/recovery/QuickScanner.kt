package com.example.recovery

import android.os.Environment
import java.io.File

/** المسح السريع بدون root: يتطلب صلاحية MANAGE_EXTERNAL_STORAGE. */
object QuickScanner {
    private val trashDirs = setOf(".thumbnails", ".trash", ".Trash")

    fun find(): List<String> =
        Environment.getExternalStorageDirectory().walkTopDown()
            .onEnter { it.name != "Android" }
            .filter { it.isFile && (it.name.startsWith(".trashed-") || it.parentFile?.name in trashDirs) }
            .map { it.absolutePath }
            .toList()
}
