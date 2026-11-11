package com.example.recovery.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.LayoutDirection

val Ink = Color(0xFF14232B)
val Paper = Color(0xFFF3F5F4)
val Signal = Color(0xFF0E7C86)
val Muted = Color(0xFF5E6E75)
val Warn = Color(0xFF8A5A0B)

@Composable
fun RecoveryTheme(content: @Composable () -> Unit) {
    CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Rtl) {
        MaterialTheme(
            colorScheme = lightColorScheme(
                primary = Signal, onPrimary = Color.White,
                background = Paper, surface = Paper,
                onBackground = Ink, onSurface = Ink,
            ),
            content = content,
        )
    }
}
