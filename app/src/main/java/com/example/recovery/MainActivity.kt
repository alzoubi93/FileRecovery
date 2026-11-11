package com.example.recovery

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.activity.viewModels
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.example.recovery.ui.RecoveryApp
import com.example.recovery.ui.RecoveryTheme

class MainActivity : ComponentActivity() {
    private val vm: RecoveryViewModel by viewModels()
    private var hasAllFiles by mutableStateOf(false)

    private val mediaPermLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
            if (granted.values.all { it }) vm.setProtection(true)
            else vm.showToast("تحتاج الحماية الاستباقية إلى إذن الوصول للصور والفيديو")
        }

    private fun mediaPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= 33)
            arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO)
        else arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)

    private fun onToggleProtection(on: Boolean) {
        if (!on) { vm.setProtection(false); return }
        val missing = mediaPermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) vm.setProtection(true) else mediaPermLauncher.launch(missing.toTypedArray())
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            RecoveryTheme { RecoveryApp(vm, hasAllFiles, ::requestAllFiles, ::onToggleProtection) }
        }
    }

    override fun onResume() {
        super.onResume()
        hasAllFiles = Environment.isExternalStorageManager()
        vm.refreshProtection()
    }

    private fun requestAllFiles() {
        startActivity(
            Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:$packageName"))
        )
    }
}
