package com.auris.app

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat

class MainActivity : ComponentActivity() {

    private var ble: AurisBle? = null
    private val bleState = mutableStateOf(AurisState.IDLE)
    private val logs = mutableStateListOf<String>()
    private val lastNotif = mutableStateOf("")
    private val playerStatus = mutableStateOf(AurisPlayerStatus())

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        if (results.values.all { it }) {
            ble?.startScan()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        ble = AurisBle(
            context = this,
            onStateChange = { runOnUiThread { bleState.value = it } },
            onNotification = { runOnUiThread { lastNotif.value = it } },
            onLog = { msg -> runOnUiThread { logs.add(msg); if (logs.size > 200) logs.removeAt(0) } },
            onStatus = { st -> runOnUiThread { playerStatus.value = st } },
        )

        setContent {
            AurisTheme {
                AurisScreen(
                    state = bleState.value,
                    logs = logs,
                    lastNotif = lastNotif.value,
                    status = playerStatus.value,
                    onConnect = ::connect,
                    onDisconnect = { ble?.disconnect() },
                    onCommand = { ble?.sendCommand(it) },
                )
            }
        }
    }

    private fun connect() {
        val perms = arrayOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.ACCESS_FINE_LOCATION,
        )
        val missing = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            ble?.startScan()
        } else {
            permLauncher.launch(missing.toTypedArray())
        }
    }

    override fun onDestroy() {
        ble?.destroy()
        super.onDestroy()
    }
}

// ── Theme ──

private val DarkGold = Color(0xFFD4A847)
private val DarkBg = Color(0xFF121212)
private val SurfaceDark = Color(0xFF1E1E1E)

@Composable
fun AurisTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = DarkGold,
            onPrimary = Color.Black,
            surface = SurfaceDark,
            background = DarkBg,
            onBackground = Color.White,
            onSurface = Color.White,
        ),
        content = content,
    )
}

// ── Screen ──

@Composable
fun AurisScreen(
    state: AurisState,
    logs: List<String>,
    lastNotif: String,
    status: AurisPlayerStatus,
    onConnect: () -> Unit,
    onDisconnect: () -> Unit,
    onCommand: (String) -> Unit,
) {
    Scaffold(
        containerColor = DarkBg,
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 20.dp, vertical = 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            // Header
            Text(
                "AURIS",
                fontSize = 28.sp,
                fontWeight = FontWeight.Bold,
                color = DarkGold,
                letterSpacing = 8.sp,
            )

            Spacer(Modifier.height(4.dp))
            StatusChip(state)
            Spacer(Modifier.height(8.dp))

            // Notification banner
            if (lastNotif.isNotEmpty() && !lastNotif.startsWith("OK:")) {
                Surface(
                    shape = RoundedCornerShape(12.dp),
                    color = DarkGold.copy(alpha = 0.15f),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(
                        lastNotif,
                        modifier = Modifier.padding(12.dp),
                        color = DarkGold,
                        fontWeight = FontWeight.Medium,
                        fontSize = 15.sp,
                    )
                }
                Spacer(Modifier.height(8.dp))
            }

            // Connect / disconnect
            if (state == AurisState.IDLE) {
                Button(
                    onClick = onConnect,
                    modifier = Modifier.fillMaxWidth().height(52.dp),
                    shape = RoundedCornerShape(14.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = DarkGold),
                ) {
                    Text("Connect to AURIS", color = Color.Black, fontWeight = FontWeight.Bold)
                }
            } else if (state == AurisState.SCANNING || state == AurisState.CONNECTING) {
                CircularProgressIndicator(color = DarkGold)
                Spacer(Modifier.height(8.dp))
                Text(
                    if (state == AurisState.SCANNING) "Scanning..." else "Connecting...",
                    color = Color.Gray,
                )
            }

            // Player status + controls
            if (state == AurisState.READY || state == AurisState.CONNECTED) {
                PlayerCard(status, onCommand)
                Spacer(Modifier.height(8.dp))
                ControlRow(onCommand, onDisconnect)
            }

            Spacer(Modifier.height(8.dp))

            // Log console
            Text("Console", color = Color.Gray, fontSize = 12.sp, fontWeight = FontWeight.Medium)
            Spacer(Modifier.height(4.dp))

            val listState = rememberLazyListState()
            LaunchedEffect(logs.size) {
                if (logs.isNotEmpty()) listState.animateScrollToItem(logs.size - 1)
            }

            Surface(
                shape = RoundedCornerShape(10.dp),
                color = Color(0xFF0D0D0D),
                modifier = Modifier.fillMaxWidth().weight(1f),
            ) {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.padding(10.dp),
                ) {
                    items(logs) { line ->
                        Text(
                            line,
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace,
                            color = if (line.startsWith("←")) DarkGold
                                    else if (line.startsWith("→")) Color(0xFF6CBF6C)
                                    else Color(0xFFAAAAAA),
                            lineHeight = 16.sp,
                        )
                    }
                }
            }
        }
    }
}

// ── Player card ──

@Composable
fun PlayerCard(status: AurisPlayerStatus, onCommand: (String) -> Unit) {
    Surface(
        shape = RoundedCornerShape(14.dp),
        color = Color(0xFF1A1A1A),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(16.dp)) {
            // Track name
            Text(
                status.track.ifEmpty { "No track" },
                fontSize = 16.sp,
                fontWeight = FontWeight.Bold,
                color = Color.White,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )

            Spacer(Modifier.height(4.dp))

            // Phase + track info
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    status.phase,
                    fontSize = 12.sp,
                    color = when (status.phase) {
                        "PLAYING" -> Color(0xFF66BB6A)
                        "PAUSED" -> Color(0xFFFFA726)
                        "ERROR" -> Color(0xFFEF5350)
                        else -> Color.Gray
                    },
                    fontWeight = FontWeight.Medium,
                )
                if (status.trackCount > 0) {
                    Spacer(Modifier.width(8.dp))
                    Text(
                        "${status.trackIndex + 1}/${status.trackCount}",
                        fontSize = 12.sp,
                        color = Color.Gray,
                    )
                }
                if (status.bitrateKbps > 0) {
                    Spacer(Modifier.width(8.dp))
                    Text(
                        "${status.bitrateKbps}kbps",
                        fontSize = 12.sp,
                        color = Color.Gray,
                    )
                }
            }

            Spacer(Modifier.height(8.dp))

            // Progress bar
            val progress = if (status.durationSec > 0)
                status.positionSec.toFloat() / status.durationSec else 0f

            LinearProgressIndicator(
                progress = { progress },
                modifier = Modifier.fillMaxWidth().height(4.dp),
                color = DarkGold,
                trackColor = Color(0xFF333333),
            )

            Spacer(Modifier.height(4.dp))

            // Time
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text(formatTime(status.positionSec), fontSize = 12.sp, color = Color.Gray)
                Text(formatTime(status.durationSec), fontSize = 12.sp, color = Color.Gray)
            }

            Spacer(Modifier.height(8.dp))

            // Volume
            Row(
                Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center,
            ) {
                CmdBtn("−", Color.Gray) { onCommand("VOL-") }
                Spacer(Modifier.width(12.dp))
                Text("Vol ${status.volume}%", fontSize = 13.sp, color = Color.White)
                Spacer(Modifier.width(12.dp))
                CmdBtn("+", Color.Gray) { onCommand("VOL+") }
            }
        }
    }
}

@Composable
fun CmdBtn(label: String, color: Color, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        modifier = Modifier.size(36.dp),
        shape = RoundedCornerShape(8.dp),
        colors = ButtonDefaults.buttonColors(containerColor = color.copy(alpha = 0.2f)),
        contentPadding = PaddingValues(0.dp),
    ) {
        Text(label, color = color, fontWeight = FontWeight.Bold, fontSize = 16.sp)
    }
}

// ── Control row ──

@Composable
fun ControlRow(onCommand: (String) -> Unit, onDisconnect: () -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CmdButton(Modifier.weight(1f), "PREV", "⏮", Color(0xFF42A5F5), onCommand)
            CmdButton(Modifier.weight(1f), "PLAY", "▶", DarkGold, onCommand)
            CmdButton(Modifier.weight(1f), "PAUSE", "⏸", Color(0xFFFFA726), onCommand)
            CmdButton(Modifier.weight(1f), "NEXT", "⏭", Color(0xFF66BB6A), onCommand)
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CmdButton(Modifier.weight(1f), "RESTART", "↺", Color(0xFF42A5F5), onCommand)
            CmdButton(Modifier.weight(1f), "UNPAIR", "✕", Color(0xFFEF5350), onCommand)
        }
        OutlinedButton(
            onClick = onDisconnect,
            modifier = Modifier.fillMaxWidth().height(40.dp),
            shape = RoundedCornerShape(12.dp),
            colors = ButtonDefaults.outlinedButtonColors(contentColor = Color.Gray),
        ) {
            Text("Disconnect", fontSize = 13.sp)
        }
    }
}

@Composable
fun StatusChip(state: AurisState) {
    val (label, color) = when (state) {
        AurisState.IDLE -> "Disconnected" to Color.Gray
        AurisState.SCANNING -> "Scanning" to Color(0xFFFFA726)
        AurisState.CONNECTING -> "Connecting" to Color(0xFFFFA726)
        AurisState.CONNECTED -> "Connected" to Color(0xFF66BB6A)
        AurisState.READY -> "Ready" to DarkGold
    }
    Surface(
        shape = RoundedCornerShape(20.dp),
        color = color.copy(alpha = 0.2f),
    ) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            color = color,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

@Composable
fun CmdButton(
    modifier: Modifier,
    cmd: String,
    symbol: String,
    color: Color,
    onCommand: (String) -> Unit,
) {
    Button(
        onClick = { onCommand(cmd) },
        modifier = modifier.height(48.dp),
        shape = RoundedCornerShape(12.dp),
        colors = ButtonDefaults.buttonColors(containerColor = color.copy(alpha = 0.15f)),
        contentPadding = PaddingValues(horizontal = 4.dp),
    ) {
        Text(symbol, color = color, fontSize = 16.sp)
        Spacer(Modifier.width(4.dp))
        Text(cmd, color = color, fontWeight = FontWeight.Bold, fontSize = 11.sp)
    }
}

private fun formatTime(seconds: Int): String {
    val m = seconds / 60
    val s = seconds % 60
    return "%d:%02d".format(m, s)
}
