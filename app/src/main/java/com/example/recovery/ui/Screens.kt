package com.example.recovery.ui

import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import com.example.recovery.RecoveryViewModel
import com.example.recovery.Screen
import com.example.recovery.UiState

private val previewExt = setOf("jpg", "jpeg", "png", "webp", "gif", "heic", "heif", "bmp")

@Composable
fun RecoveryApp(
    vm: RecoveryViewModel, hasAllFiles: Boolean, onGrant: () -> Unit, onToggleProtection: (Boolean) -> Unit,
) {
    val s by vm.state.collectAsState()
    val ctx = LocalContext.current
    LaunchedEffect(s.toast) {
        s.toast?.let { Toast.makeText(ctx, it, Toast.LENGTH_LONG).show(); vm.consumeToast() }
    }
    when (s.screen) {
        Screen.Home -> HomeScreen(
            s, hasAllFiles, onGrant, vm::startQuickScan, vm::startDeepScan, onToggleProtection, vm::showProtected,
        )
        Screen.Scanning -> ScanningScreen(s, vm::cancelScan)
        Screen.Results -> ResultsScreen(s, vm::toggle, vm::selectAll, vm::saveSelected, vm::backHome)
    }
}

@Composable
fun HomeScreen(
    s: UiState, hasAllFiles: Boolean, onGrant: () -> Unit,
    onQuick: () -> Unit, onDeep: (String) -> Unit,
    onToggleProtection: (Boolean) -> Unit, onShowProtected: () -> Unit,
) {
    var picked by remember { mutableStateOf<String?>(null) }
    Column(
        Modifier.fillMaxSize().background(Paper).statusBarsPadding()
            .verticalScroll(rememberScrollState()).padding(24.dp)
    ) {
        Text("استعادة الملفات", fontSize = 30.sp, fontWeight = FontWeight.Bold, color = Ink)
        Spacer(Modifier.height(6.dp))
        Text(
            if (s.rootAvailable) "صلاحية root متاحة" else "root غير متاح: المسح السريع فقط",
            color = if (s.rootAvailable) Signal else Muted,
        )
        if (s.rootAvailable && s.encrypted) {
            Spacer(Modifier.height(10.dp))
            Text(
                "الجهاز مشفّر (FBE): المسح العميق لقسم البيانات لن يعطي صوراً سليمة. " +
                    "استخدمه مع بطاقة SD أو الأقسام غير المشفّرة.",
                color = Warn, fontSize = 14.sp,
            )
        }

        Spacer(Modifier.height(32.dp))
        Text("المسح السريع", fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(4.dp))
        Text("يبحث في سلة المحذوفات والصور المصغّرة والمجلدات المخفية.", color = Muted)
        Spacer(Modifier.height(12.dp))
        if (hasAllFiles || s.rootAvailable) {
            Button(onClick = onQuick) { Text("ابدأ المسح السريع") }
        } else {
            OutlinedButton(onClick = onGrant) { Text("امنح صلاحية الوصول للملفات") }
        }

        Spacer(Modifier.height(24.dp)); HorizontalDivider(); Spacer(Modifier.height(24.dp))

        Text("المسح العميق", fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(4.dp))
        if (!s.rootAvailable) {
            Text("يتطلب صلاحية root.", color = Muted)
        } else {
            Text("اختر القسم الذي تريد فحصه بحثاً عن صور وفيديو ومستندات محذوفة.", color = Muted)
            Spacer(Modifier.height(8.dp))
            if (s.partitions.isEmpty()) Text("لم يُعثر على أقسام قابلة للفحص.", color = Muted)
            s.partitions.forEach { p ->
                val usable = !p.encrypted
                Row(
                    Modifier.fillMaxWidth().clickable(enabled = usable) { picked = p.device }.padding(vertical = 2.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    RadioButton(selected = picked == p.device, enabled = usable, onClick = { picked = p.device })
                    Column {
                        Text(p.mount, fontWeight = FontWeight.Medium, color = if (usable) Ink else Muted)
                        Text(
                            p.device + " · " + p.fs + if (p.encrypted) " · مشفّر (لا جدوى من المسح)" else "",
                            fontFamily = FontFamily.Monospace, fontSize = 12.sp,
                            color = if (p.encrypted) Warn else Muted,
                        )
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
            Button(enabled = picked != null, onClick = { picked?.let(onDeep) }) {
                Text("ابدأ المسح العميق")
            }
        }

        Spacer(Modifier.height(24.dp)); HorizontalDivider(); Spacer(Modifier.height(24.dp))

        Text("الحماية الاستباقية", fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(4.dp))
        Text(
            "تحتفظ بنسخة من صورك وفيديوهاتك الجديدة، فإن حذفتَ شيئاً تجده هنا. " +
                "هذه أضمن طريقة على الأجهزة المشفّرة.",
            color = Muted,
        )
        Spacer(Modifier.height(10.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Switch(checked = s.protectionOn, onCheckedChange = onToggleProtection)
            Spacer(Modifier.width(12.dp))
            Text(if (s.protectionOn) "مفعّلة" else "متوقفة")
        }
        if (s.protectionOn) {
            Spacer(Modifier.height(8.dp))
            Text(
                "${s.stats.protectedCount} ملف محمي · %.0f ميغابايت · ${s.stats.deletedCount} محذوف يمكن استعادته"
                    .format(s.stats.bytes / 1048576.0),
                color = Muted, fontSize = 14.sp,
            )
            if (s.stats.deletedCount > 0) {
                Spacer(Modifier.height(10.dp))
                Button(onClick = onShowProtected) { Text("عرض المحذوفات المحمية") }
            }
        }
        Spacer(Modifier.height(8.dp))
        Text(
            "تستهلك حتى 2 غيغابايت، وتنسخ ملفات آخر 30 يوماً عند التفعيل، وتُحذف عند إلغاء تثبيت التطبيق. " +
                "المحذوفات تُحفظ 60 يوماً.",
            color = Muted, fontSize = 12.sp,
        )
    }
}

@Composable
fun ScanningScreen(s: UiState, onCancel: () -> Unit) {
    val angle by rememberInfiniteTransition(label = "scan").animateFloat(
        initialValue = 0f, targetValue = 360f,
        animationSpec = infiniteRepeatable(tween(1800, easing = LinearEasing), RepeatMode.Restart),
        label = "angle",
    )
    Column(
        Modifier.fillMaxSize().background(Paper).padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box(Modifier.size(220.dp), contentAlignment = Alignment.Center) {
            Canvas(Modifier.fillMaxSize().padding(8.dp)) {
                val stroke = Stroke(width = 10.dp.toPx(), cap = StrokeCap.Round)
                drawArc(Signal.copy(alpha = 0.15f), 0f, 360f, false, style = stroke, size = Size(size.width, size.height))
                drawArc(Signal, angle, 90f, false, style = stroke, size = Size(size.width, size.height))
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("${s.found}", fontSize = 56.sp, fontWeight = FontWeight.Bold, color = Ink)
                Text("ملف عُثر عليه", color = Muted)
            }
        }
        Spacer(Modifier.height(28.dp))
        Text(s.label, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
        if (s.bytes > 0) {
            Spacer(Modifier.height(4.dp))
            Text("%.1f ميغابايت تم فحصها".format(s.bytes / 1048576.0), color = Muted)
        }
        Spacer(Modifier.height(28.dp))
        OutlinedButton(onClick = onCancel) { Text("إيقاف وعرض النتائج") }
    }
}

@Composable
fun ResultsScreen(
    s: UiState, onToggle: (String) -> Unit, onSelectAll: () -> Unit,
    onSave: () -> Unit, onBack: () -> Unit,
) {
    BackHandler(onBack = onBack)
    Column(Modifier.fillMaxSize().background(Paper).statusBarsPadding()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("${s.results.size} ملف", fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
            TextButton(onClick = onSelectAll) { Text("تحديد الكل") }
        }
        if (s.results.isEmpty()) {
            Box(Modifier.weight(1f).fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) {
                Text(
                    "لم يُعثر على ملفات قابلة للاستعادة. على الأجهزة المشفّرة تبقى سلة المحذوفات " +
                        "والمجلدات المخفية أنجح المصادر.",
                    color = Muted, textAlign = TextAlign.Center,
                )
            }
        } else {
            LazyVerticalGrid(
                columns = GridCells.Fixed(3),
                modifier = Modifier.weight(1f),
                contentPadding = PaddingValues(2.dp),
                horizontalArrangement = Arrangement.spacedBy(2.dp),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                items(s.results, key = { it.absolutePath }) { f ->
                    val sel = f.absolutePath in s.selected
                    Box(Modifier.aspectRatio(1f).clickable { onToggle(f.absolutePath) }) {
                        if (f.extension.lowercase() in previewExt) {
                            AsyncImage(
                                model = f, contentDescription = null,
                                contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize(),
                            )
                        } else {
                            Box(Modifier.fillMaxSize().background(Signal.copy(alpha = 0.12f)), contentAlignment = Alignment.Center) {
                                Text(f.extension.uppercase(), fontWeight = FontWeight.Bold, color = Signal, fontSize = 18.sp)
                            }
                        }
                        if (sel) {
                            Box(Modifier.fillMaxSize().background(Signal.copy(alpha = 0.35f)))
                            Icon(
                                Icons.Default.Check, contentDescription = "محدد", tint = Color.White,
                                modifier = Modifier.align(Alignment.TopStart).padding(6.dp)
                                    .clip(androidx.compose.foundation.shape.CircleShape)
                                    .background(Signal).padding(2.dp),
                            )
                        }
                    }
                }
            }
        }
        Button(
            onClick = onSave, enabled = s.selected.isNotEmpty(),
            modifier = Modifier.fillMaxWidth().navigationBarsPadding().padding(16.dp),
        ) { Text("حفظ المحدد (${s.selected.size})") }
    }
}
