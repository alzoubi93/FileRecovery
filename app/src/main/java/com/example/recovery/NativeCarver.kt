package com.example.recovery

import android.os.ParcelFileDescriptor
import java.io.File

object NativeCarver {
    init { System.loadLibrary("recovery") }

    private external fun carve(fd: Int, outDir: String): Int
    private external fun carveRootDevice(device: String, outDir: String): Int
    external fun progressBytes(): Long
    external fun progressCount(): Int
    external fun cancel()

    /** مسح ملف أو صورة قرص (بدون root). استدعِها من خيط خلفي (Dispatchers.IO). */
    fun carveFile(source: File, outDir: File): Int {
        outDir.mkdirs()
        return ParcelFileDescriptor.open(source, ParcelFileDescriptor.MODE_READ_ONLY).use {
            carve(it.fd, outDir.absolutePath)
        }
    }

    /** مسح عميق لقسم كامل بصلاحية root. مثال: /dev/block/mmcblk1p1 */
    fun carveDevice(devicePath: String, outDir: File): Int {
        outDir.mkdirs()
        return carveRootDevice(devicePath, outDir.absolutePath)
    }
}
