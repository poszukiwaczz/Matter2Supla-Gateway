package pl.matter2supla.finder

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Logout
import androidx.compose.material.icons.filled.AddCircleOutline
import androidx.compose.material.icons.filled.Dashboard
import androidx.compose.material.icons.filled.Devices
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.PowerSettingsNew
import androidx.compose.material.icons.filled.QrCodeScanner
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

private val BgDark = Color(0xFF081622)
private val CardBg = Color(0xFF102131)
private val CardBorder = Color(0xFF294052)
private val AccentGreen = Color(0xFF55D47A)
private val TextMuted = Color(0xFF9EB3C2)
private val WarnAmber = Color(0xFFFFD166)
private val ErrorRed = Color(0xFFFF6B6B)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NativeGatewayPanelScreen(
    gateway: DiscoveredGateway,
    client: GatewayApiClient,
    onBackToList: () -> Unit,
    onLogout: () -> Unit
) {
    val scope = rememberCoroutineScope()
    val lifecycleOwner = LocalLifecycleOwner.current
    var selectedTab by remember { mutableIntStateOf(0) }
    var loading by remember { mutableStateOf(false) }
    var errorMsg by remember { mutableStateOf<String?>(null) }

    var statusJson by remember { mutableStateOf<JSONObject?>(null) }
    var settingsJson by remember { mutableStateOf<JSONObject?>(null) }
    var devicesJson by remember { mutableStateOf<JSONArray?>(null) }
    var diagJson by remember { mutableStateOf<JSONObject?>(null) }

    var refreshRequest by remember { mutableIntStateOf(0) }
    var pairingAttempt by remember { mutableStateOf<PairingAttempt?>(null) }
    var readingsStale by remember { mutableStateOf(false) }
    var pendingRelay by remember { mutableStateOf<PendingRelay?>(null) }
    var relayFeedback by remember { mutableStateOf<String?>(null) }

    fun refreshCurrentTab() {
        refreshRequest++
    }

    fun sendRelay(node: Long, endpoint: Int, on: Boolean) {
        if (pendingRelay != null || readingsStale) return
        val action = PendingRelay(node, endpoint, on, android.os.SystemClock.elapsedRealtime())
        pendingRelay = action
        relayFeedback = "Wysyłam polecenie do gniazdka…"
        scope.launch {
            val result = client.controlRelay(node, endpoint, on)
            if (pendingRelay != action) return@launch
            if (!result.isOk && result.code in 400..499) {
                pendingRelay = null
                relayFeedback = result.json?.optString("message")?.ifBlank { null } ?: "Błąd sterowania (HTTP " + result.code + ")."
                return@launch
            }
            relayFeedback = "Czekam na świeży odczyt stanu gniazdka…"
            if (selectedTab == 1) refreshCurrentTab()
            delay(30_000)
            if (pendingRelay == action) {
                pendingRelay = null
                relayFeedback = "Brak potwierdzenia stanu gniazdka. Sprawdź odczyt przed ponowieniem."
            }
        }
    }

    suspend fun fetchTabData(tab: Int) {
        val requestedAt = android.os.SystemClock.elapsedRealtime()
        val response = when (tab) {
            1 -> client.getDevices()
            3 -> client.getDiagnostics()
            else -> client.getStatus()
        }
        if (response.code == 401) {
            onLogout()
            return
        }
        val json = response.json
        if (!response.isOk || json == null || (tab == 1 && json.optJSONArray("devices") == null)) {
            if (tab == 1) readingsStale = true
            val detail = response.json?.optString("message")?.ifBlank { null } ?: response.error
            errorMsg = if (tab == 1 && devicesJson != null) {
                "Nie odświeżono pomiarów. Widoczne są ostatnie dostępne odczyty."
            } else detail ?: "Nie udało się odświeżyć danych (HTTP " + response.code + ")."
            return
        }
        errorMsg = null
        when (tab) {
            1 -> {
                val devices = json.optJSONArray("devices")
                devicesJson = devices
                readingsStale = false
                val action = pendingRelay
                if (action != null && devices != null && devices.confirmsRelay(action, requestedAt)) {
                    pendingRelay = null
                    relayFeedback = if (action.on) "Potwierdzony stan gniazdka: włączone." else "Potwierdzony stan gniazdka: wyłączone."
                }
            }
            3 -> diagJson = json
            else -> statusJson = json
        }
        if ((tab == 0 || tab == 3) && settingsJson == null) {
            val settings = client.get("/api/gateway/settings")
            if (settings.isOk) settingsJson = settings.json
        }
    }

    LaunchedEffect(lifecycleOwner, selectedTab, refreshRequest, client) {
        val tab = selectedTab
        lifecycleOwner.lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
            loading = true
            try {
                fetchTabData(tab)
            } finally {
                loading = false
            }
            while (isActive) {
                delay(5000L)
                fetchTabData(tab)
            }
        }
    }

    val gwTitle = settingsJson?.optString("gateway_name")?.ifEmpty { null }
        ?: statusJson?.optJSONObject("system")?.optString("gateway_name")?.ifEmpty { null }
        ?: gateway.name

    Scaffold(
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
        topBar = {
            TopAppBar(
                windowInsets = WindowInsets(0, 0, 0, 0),
                title = {
                    Column {
                        Text(gwTitle, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                        Text("${gateway.ip} · LIVE", style = MaterialTheme.typography.bodySmall, color = AccentGreen)
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onBackToList) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Lista bramek")
                    }
                },
                actions = {
                    IconButton(onClick = { refreshCurrentTab() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Odśwież")
                    }
                    IconButton(onClick = onLogout) {
                        Icon(Icons.AutoMirrored.Filled.Logout, contentDescription = "Wyloguj")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = BgDark,
                    titleContentColor = Color(0xFFF4F7FB),
                    navigationIconContentColor = Color(0xFFF4F7FB),
                    actionIconContentColor = AccentGreen
                )
            )
        },
        bottomBar = {
            NavigationBar(
                containerColor = Color(0xFF0C1D2B),
                windowInsets = WindowInsets(0, 0, 0, 0)
            ) {
                NavigationBarItem(
                    selected = selectedTab == 0,
                    onClick = { selectedTab = 0 },
                    icon = { Icon(Icons.Default.Dashboard, contentDescription = null) },
                    label = { Text("Pulpit") }
                )
                NavigationBarItem(
                    selected = selectedTab == 1,
                    onClick = { selectedTab = 1 },
                    icon = { Icon(Icons.Default.Devices, contentDescription = null) },
                    label = { Text("Urządzenia") }
                )
                NavigationBarItem(
                    selected = selectedTab == 2,
                    onClick = { selectedTab = 2 },
                    icon = { Icon(Icons.Default.AddCircleOutline, contentDescription = null) },
                    label = { Text("Dodaj") }
                )
                NavigationBarItem(
                    selected = selectedTab == 3,
                    onClick = { selectedTab = 3 },
                    icon = { Icon(Icons.Default.Memory, contentDescription = null) },
                    label = { Text("Diagnostyka") }
                )
            }
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
        ) {
            if (loading) {
                LinearProgressIndicator(
                    modifier = Modifier.fillMaxWidth(),
                    color = AccentGreen,
                    trackColor = Color(0xFF14283A)
                )
            }
            if (errorMsg != null) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF3A161E)),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Text(
                        text = errorMsg!!,
                        color = ErrorRed,
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }

            when (selectedTab) {
                0 -> DashboardTab(statusJson, gwTitle)
                1 -> DevicesTab(
                    devices = devicesJson,
                    actionFeedback = relayFeedback,
                    canControl = !readingsStale && pendingRelay == null,
                    onControl = ::sendRelay
                )
                2 -> PairMatterTab(
                    client = client,
                    statusJson = statusJson,
                    pairingAttempt = pairingAttempt,
                    onPairingAttempt = { pairingAttempt = it },
                    onRefreshStatus = { refreshCurrentTab() }
                )
                3 -> DiagnosticsTab(
                    diag = diagJson,
                    settings = settingsJson,
                    client = client,
                    onLogout = onLogout
                )
            }
        }
    }
}

@Composable
private fun DashboardTab(status: JSONObject?, gatewayName: String) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        if (status == null) {
            Text("Ładowanie stanu bramki...", color = TextMuted)
            return
        }
        val sys = status.optJSONObject("system")
        val wifi = status.optJSONObject("wifi")
        val thread = status.optJSONObject("thread")
        val supla = status.optJSONObject("supla")

        InfoCard("System") {
            KeyVal("Nazwa bramki", gatewayName)
            KeyVal("Firmware", sys?.optString("version", "-") ?: "-")
            KeyVal("Czas działania", formatUptime(sys?.optLong("uptime", 0L) ?: 0L))
            KeyVal("Wolny Heap (ogółem)", formatBytes(sys?.optLong("heap", 0L) ?: 0L))
            KeyVal("Urządzenia Matter", (sys?.optInt("devices", 0) ?: 0).toString())
        }

        InfoCard("Wi-Fi") {
            val connected = wifi?.optBoolean("connected", false) == true
            KeyVal("Status", if (connected) "Połączono" else "Rozłączono", if (connected) AccentGreen else ErrorRed)
            KeyVal("Tryb", wifi?.optString("mode", "-")?.uppercase() ?: "-")
            KeyVal("Sieć (SSID)", wifi?.optString("ssid", "-") ?: "-")
            KeyVal("Adres IP", wifi?.optString("ip", "-") ?: "-")
            if (wifi != null && wifi.has("rssi")) {
                KeyVal("Sygnał RSSI", "${wifi.optInt("rssi")} dBm")
            }
        }

        InfoCard("Sieć Thread") {
            KeyVal("Rola", thread?.optString("role", "-") ?: "-")
            KeyVal("Kanał", thread?.optInt("channel", 0)?.takeIf { it > 0 }?.toString() ?: "-")
            val hasDs = thread?.optBoolean("dataset", false) == true
            KeyVal("Dataset", if (hasDs) "Aktywny" else "Brak", if (hasDs) AccentGreen else WarnAmber)
        }

        InfoCard("SUPLA Cloud") {
            val code = supla?.optInt("code", -1) ?: -1
            val state = supla?.optString("state", "-") ?: "-"
            KeyVal("Stan", "$state (kod $code)", if (code == 17) AccentGreen else WarnAmber)
        }
    }
}

private data class ParsedMetric(
    val label: String,
    val formattedValue: String,
    val details: String,
    val isValid: Boolean
)

private data class PendingRelay(val node: Long, val endpoint: Int, val on: Boolean, val sentAtMs: Long)

private fun JSONArray.confirmsRelay(action: PendingRelay, requestedAtMs: Long): Boolean {
    if (requestedAtMs < action.sentAtMs) return false
    for (index in 0 until length()) {
        val device = optJSONObject(index) ?: continue
        if (device.optLong("node") != action.node) continue
        val values = device.optJSONObject("telemetry")?.optJSONArray("values") ?: return false
        for (readingIndex in 0 until values.length()) {
            val reading = values.optJSONObject(readingIndex) ?: continue
            val kind = reading.optString("kind", reading.optString("metric", ""))
            val ageSeconds = reading.optLong("received_age_s", -1)
            if ((kind == "onoff" || kind == "on_off") && reading.optInt("endpoint") == action.endpoint &&
                reading.optBoolean("valid") && reading.optDouble("value") == (if (action.on) 1.0 else 0.0) &&
                ageSeconds >= 0 && ageSeconds <= (requestedAtMs - action.sentAtMs) / 1000) return true
        }
        return false
    }
    return false
}

private data class RelayControl(
    val endpoint: Int,
    val isOn: Boolean?
)

@Composable
private fun DevicesTab(
    devices: JSONArray?,
    actionFeedback: String?,
    canControl: Boolean,
    onControl: (Long, Int, Boolean) -> Unit
) {

    val list = remember(devices) {
        buildList {
            if (devices != null) {
                for (i in 0 until devices.length()) {
                    devices.optJSONObject(i)?.let { add(it) }
                }
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        if (actionFeedback != null) {
            Card(
                colors = CardDefaults.cardColors(containerColor = Color(0xFF14283A)),
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(bottom = 10.dp)
            ) {
                Text(
                    text = actionFeedback!!,
                    color = AccentGreen,
                    modifier = Modifier.padding(12.dp),
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }

        if (list.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(if (devices == null) "Brak aktualnej listy urządzeń." else "Brak sparowanych urządzeń Matter.", color = TextMuted)
            }
            return
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            items(list, key = { it.optLong("node") }) { dev ->
                val node = dev.optLong("node")
                val name = dev.optString("name", "Node $node")
                val vid = dev.optInt("vid", 0)
                val pid = dev.optInt("pid", 0)
                val tel = dev.optJSONObject("telemetry")
                val subActive = tel?.optBoolean("subscription_active", false) == true
                val reportCount = tel?.optLong("report_count", 0L) ?: 0L
                val lastReportAge = if (tel != null && !tel.isNull("last_report_age_s")) tel.optInt("last_report_age_s", -1) else -1
                val lastContactAge = if (tel != null && !tel.isNull("last_contact_age_s")) tel.optInt("last_contact_age_s", -1) else -1
                val minS = if (tel != null && tel.has("minimum_s")) tel.optInt("minimum_s", -1) else -1
                val maxS = if (tel != null && tel.has("maximum_s")) tel.optInt("maximum_s", -1) else -1

                val values = tel?.optJSONArray("values")
                val threadInfo = tel?.optJSONObject("thread")

                val relayControls = mutableListOf<RelayControl>()
                val parsedMetrics = mutableListOf<ParsedMetric>()

                if (values != null) {
                    for (i in 0 until values.length()) {
                        val v = values.optJSONObject(i) ?: continue
                        // beta4 uses "kind"; fallback to "metric" for compatibility
                        val kind = v.optString("kind", v.optString("metric", "")).lowercase(Locale.ROOT)
                        val ep = v.optInt("endpoint", 1)
                        val valid = v.optBoolean("valid", !v.isNull("value"))
                        val unit = v.optString("unit", "")
                        val rawStr = if (!v.isNull("raw")) v.optString("raw", "") else ""
                        val numVal = if (!v.isNull("value")) v.optDouble("value", Double.NaN) else Double.NaN
                        val src = v.optString("source", "")
                        val recAge = if (!v.isNull("received_age_s")) v.optInt("received_age_s", -1) else -1
                        val suplaCh = if (v.has("supla_channel")) v.optInt("supla_channel", -1) else -1

                        val label = when (kind) {
                            "temperature" -> "Temperatura (EP$ep)"
                            "humidity" -> "Wilgotność (EP$ep)"
                            "battery" -> "Bateria (EP$ep)"
                            "onoff", "on_off" -> "Przekaźnik OnOff (EP$ep)"
                            "voltage" -> "Napięcie (EP$ep)"
                            "current" -> "Natężenie prądu (EP$ep)"
                            "active_power" -> "Moc czynna (EP$ep)"
                            "imported_energy" -> "Energia pobrana (EP$ep)"
                            "exported_energy" -> "Energia oddana (EP$ep)"
                            else -> "${kind.ifEmpty { "Pomiar" }} (EP$ep)"
                        }

                        val formatted = if (!valid || numVal.isNaN()) {
                            if (kind == "onoff" || kind == "on_off") {
                                if (relayControls.none { it.endpoint == ep }) {
                                    relayControls.add(RelayControl(ep, null))
                                }
                            }
                            "Brak odczytu"
                        } else if (kind == "onoff" || kind == "on_off") {
                            val isOn = numVal >= 0.5 || rawStr == "1"
                            relayControls.removeAll { it.endpoint == ep }
                            relayControls.add(RelayControl(ep, isOn))
                            if (isOn) "WŁĄCZONE (ON)" else "WYŁĄCZONE (OFF)"
                        } else {
                            val formattedNum = when (kind) {
                                "imported_energy", "exported_energy" -> String.format(Locale.US, "%.3f", numVal)
                                "current" -> String.format(Locale.US, "%.3f", numVal)
                                "voltage", "active_power" -> String.format(Locale.US, "%.1f", numVal)
                                "temperature", "humidity" -> String.format(Locale.US, "%.2f", numVal)
                                else -> String.format(Locale.US, "%.1f", numVal)
                            }
                            if (unit.isNotEmpty()) "$formattedNum $unit" else formattedNum
                        }

                        val detailParts = mutableListOf<String>()
                        if (suplaCh >= 0) detailParts.add("SUPLA kanał #$suplaCh")
                        if (recAge >= 0) detailParts.add("odebrano ${recAge}s temu")
                        if (src.isNotEmpty()) detailParts.add(if (src == "subscription") "subskrypcja" else "odczyt")

                        parsedMetrics.add(
                            ParsedMetric(
                                label = label,
                                formattedValue = formatted,
                                details = detailParts.joinToString(" · "),
                                isValid = valid
                            )
                        )
                    }
                }

                InfoCard("$name (Node $node)") {
                    if (vid > 0 || pid > 0) {
                        KeyVal("VID / PID", "$vid / $pid")
                    }
                    KeyVal(
                        "Subskrypcja Matter",
                        if (subActive) "Aktywna (raportów: $reportCount)" else "Nieaktywna (raportów: $reportCount)",
                        if (subActive) AccentGreen else WarnAmber
                    )
                    if (minS >= 0 && maxS > 0) {
                        KeyVal("Interwał raportowania", "${minS}s – ${maxS}s")
                    }
                    if (lastReportAge >= 0) {
                        KeyVal("Ostatni raport subskrypcji", "${lastReportAge} s temu")
                    }
                    if (lastContactAge >= 0) {
                        KeyVal("Ostatni kontakt z węzłem", "${lastContactAge} s temu")
                    }

                    // Thread link info
                    if (threadInfo != null) {
                        val role = threadInfo.optString("role", "")
                        val lastRssi = if (threadInfo.has("last_rssi_dbm")) threadInfo.optInt("last_rssi_dbm", 999) else 999
                        val avgRssi = if (threadInfo.has("average_rssi_dbm")) threadInfo.optInt("average_rssi_dbm", 999) else 999
                        val lqi = if (threadInfo.has("quality")) threadInfo.optInt("quality", -1) else -1
                        val src = threadInfo.optString("source", "")
                        val parentNode = threadInfo.optString("parent_node", "")
                        val reason = threadInfo.optString("reason", "")

                        if (role.isNotEmpty()) {
                            val rolePl = when (role) {
                                "leader" -> "Leader"
                                "router" -> "Router"
                                "sleepy_end_device" -> "Uśpiony czujnik (SED)"
                                "end_device" -> "Urządzenie końcowe"
                                else -> role
                            }
                            KeyVal("Rola Thread", rolePl)
                        }
                        if (lastRssi != 999) {
                            val rssiText = buildString {
                                append("$lastRssi dBm")
                                if (avgRssi != 999) append(" (śr. $avgRssi dBm)")
                                if (lqi in 0..3) append(" · LQI $lqi/3")
                            }
                            KeyVal("Sygnał Thread RSSI", rssiText, if (lastRssi >= -75) AccentGreen else WarnAmber)
                        }
                        if (src.isNotEmpty()) {
                            val srcPl = when (src) {
                                "c6_child" -> "Bezpośrednie dziecko C6"
                                "c6_router" -> "Bezpośredni sąsiad C6"
                                "matter_parent" -> "Przez router Node $parentNode"
                                else -> src
                            }
                            KeyVal("Trasa Thread", srcPl)
                        } else if (reason.isNotEmpty()) {
                            KeyVal("Trasa Thread", reason, TextMuted)
                        }
                    }

                    if (parsedMetrics.isNotEmpty()) {
                        Spacer(Modifier.height(8.dp))
                        HorizontalDivider(color = CardBorder)
                        Spacer(Modifier.height(8.dp))
                        Text(
                            text = "Odczyty i pomiary:",
                            style = MaterialTheme.typography.labelMedium,
                            color = AccentGreen,
                            fontWeight = FontWeight.SemiBold
                        )
                        Spacer(Modifier.height(4.dp))

                        parsedMetrics.forEach { m ->
                            Column(modifier = Modifier.padding(vertical = 4.dp)) {
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.SpaceBetween
                                ) {
                                    Text(m.label, style = MaterialTheme.typography.bodySmall, color = Color(0xFFF4F7FB))
                                    Text(
                                        text = m.formattedValue,
                                        style = MaterialTheme.typography.bodyMedium,
                                        color = if (m.isValid) AccentGreen else WarnAmber,
                                        fontWeight = FontWeight.Bold
                                    )
                                }
                                if (m.details.isNotEmpty()) {
                                    Text(
                                        text = m.details,
                                        style = MaterialTheme.typography.labelSmall,
                                        color = TextMuted
                                    )
                                }
                            }
                        }
                    }

                    relayControls.sortedBy { it.endpoint }.forEach { rc ->
                        Spacer(Modifier.height(10.dp))
                        val epSuffix = if (relayControls.size > 1) " (EP${rc.endpoint})" else ""
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(10.dp),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Button(
                                enabled = canControl && rc.isOn == false,
                                onClick = { onControl(node, rc.endpoint, true) },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = if (rc.isOn == true) AccentGreen else Color(0xFF1F4E38)
                                ),
                                modifier = Modifier.weight(1f)
                            ) {
                                Text(
                                    text = "Włącz$epSuffix",
                                    color = if (rc.isOn == true) Color(0xFF062111) else Color(0xFFF4F7FB),
                                    fontWeight = FontWeight.SemiBold
                                )
                            }
                            OutlinedButton(
                                enabled = canControl && rc.isOn == true,
                                onClick = { onControl(node, rc.endpoint, false) },
                                modifier = Modifier.weight(1f)
                            ) {
                                Text("Wyłącz$epSuffix")
                            }
                        }
                    }
                }
            }
        }
    }
}

private data class PairingAttempt(val node: Long, val acceptedAtMs: Long)

@Composable
private fun PairMatterTab(
    client: GatewayApiClient,
    statusJson: JSONObject?,
    pairingAttempt: PairingAttempt?,
    onPairingAttempt: (PairingAttempt?) -> Unit,
    onRefreshStatus: () -> Unit
) {
    val scope = rememberCoroutineScope()
    var code by remember { mutableStateOf("") }
    var network by remember { mutableStateOf("thread") }
    var wifiSsid by remember { mutableStateOf("") }
    var wifiPass by remember { mutableStateOf("") }
    var submitting by remember { mutableStateOf(false) }
    var resultMessage by remember { mutableStateOf<String?>(null) }
    var resultIsError by remember { mutableStateOf(false) }
    var showQrScanner by remember { mutableStateOf(false) }
    val acceptedNode = pairingAttempt?.node

    val pairing = statusJson?.optJSONObject("pairing")
    val pairState = pairing?.optString("state", "idle") ?: "idle"
    val pairMessage = pairing?.optString("message", "") ?: ""
    val pairNode = pairing?.optLong("node", 0L) ?: 0L
    val pairElapsed = pairing?.optInt("elapsed", 0) ?: 0

    val pairingBusy = pairState in setOf("started", "commissioning", "removing")

    LaunchedEffect(pairingAttempt, pairNode, pairState, pairMessage) {
        val attempt = pairingAttempt ?: return@LaunchedEffect
        when {
            pairNode == attempt.node && (pairState == "success" || pairState == "failed") -> {
                resultIsError = pairState == "failed"
                val prefix = if (resultIsError) "Nie udało się sparować urządzenia." else "Urządzenie zostało sparowane."
                resultMessage = if (pairMessage.isBlank()) prefix else "$prefix $pairMessage"
            }
            pairNode > attempt.node -> {
                resultIsError = true
                resultMessage = "Bramka rozpoczęła inną operację. Sprawdź listę urządzeń."
            }
            else -> {
                val remaining = attempt.acceptedAtMs + 180_000 - android.os.SystemClock.elapsedRealtime()
                delay(remaining.coerceAtLeast(0))
                resultIsError = true
                resultMessage = "Upłynął czas oczekiwania na wynik. Sprawdź listę urządzeń."
            }
        }
        onPairingAttempt(null)
    }

    if (showQrScanner) {
        QrScannerDialog(
            onDismiss = { showQrScanner = false },
            onQrCodeScanned = { scannedText ->
                code = scannedText
                showQrScanner = false
                resultIsError = false
                resultMessage = "Odczytano kod QR."
            }
        )
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        InfoCard("Dodaj urządzenie Matter") {
            Text(
                text = "Zeskanuj kod QR aparatem telefonu (MT:...) lub wpisz ręcznie 11/21-cyfrowy kod parowania.",
                style = MaterialTheme.typography.bodySmall,
                color = TextMuted
            )
            Spacer(Modifier.height(12.dp))

            // Prominent QR Scanner Button
            OutlinedButton(
                onClick = { showQrScanner = true },
                border = BorderStroke(1.5.dp, AccentGreen),
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(
                    imageVector = Icons.Default.QrCodeScanner,
                    contentDescription = null,
                    tint = AccentGreen,
                    modifier = Modifier.size(20.dp)
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    text = "Skanuj kod QR aparatem",
                    color = AccentGreen,
                    fontWeight = FontWeight.SemiBold
                )
            }

            Spacer(Modifier.height(10.dp))

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(
                    selected = network == "thread",
                    onClick = { network = "thread" },
                    label = { Text("Thread") }
                )
                FilterChip(
                    selected = network == "lan",
                    onClick = { network = "lan" },
                    label = { Text("LAN") }
                )
                FilterChip(
                    selected = network == "wifi",
                    onClick = { network = "wifi" },
                    label = { Text("Wi-Fi") }
                )
            }

            Spacer(Modifier.height(10.dp))

            OutlinedTextField(
                value = code,
                onValueChange = { code = it },
                label = { Text("Kod Matter (QR MT:... lub 11/21 cyfr)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth()
            )

            if (network == "wifi") {
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = wifiSsid,
                    onValueChange = { wifiSsid = it },
                    label = { Text("SSID sieci Wi-Fi") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = wifiPass,
                    onValueChange = { wifiPass = it },
                    label = { Text("Hasło Wi-Fi") },
                    visualTransformation = PasswordVisualTransformation(),
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
            }

            Spacer(Modifier.height(14.dp))

            Button(
                enabled = !submitting && acceptedNode == null && !pairingBusy &&
                    code.isNotBlank() && (network != "wifi" || wifiSsid.isNotEmpty()),
                onClick = {
                    if (submitting || acceptedNode != null || pairingBusy) return@Button
                    submitting = true
                    scope.launch {
                        resultMessage = null
                        val res = client.pairMatter(
                            code = code,
                            network = network,
                            ssid = wifiSsid.ifBlank { null },
                            password = wifiPass.ifBlank { null }
                        )
                        submitting = false
                        val msg = res.json?.optString("message")?.ifEmpty { null }
                        val startedNode = res.json?.optLong("node", 0L) ?: 0L
                        if (res.isOk && startedNode > 0) {
                            onPairingAttempt(PairingAttempt(startedNode, android.os.SystemClock.elapsedRealtime()))
                            code = ""
                            wifiPass = ""
                            resultIsError = false
                            resultMessage = "Trwa parowanie urządzenia."
                            onRefreshStatus()
                        } else {
                            resultIsError = true
                            resultMessage = msg ?: res.error ?: "Błąd parowania (HTTP ${res.code})."
                        }
                    }
                },
                colors = ButtonDefaults.buttonColors(containerColor = AccentGreen),
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    text = when {
                        submitting -> "Wysyłanie…"
                        acceptedNode != null || pairingBusy -> "Trwa operacja Matter…"
                        else -> "Sparuj urządzenie"
                    },
                    color = Color(0xFF062111),
                    fontWeight = FontWeight.SemiBold
                )
            }

            if (resultMessage != null) {
                Spacer(Modifier.height(10.dp))
                Text(
                    text = resultMessage!!,
                    color = if (resultIsError) ErrorRed else AccentGreen,
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }

        InfoCard("Status parowania na bramce") {
            val stateLabel = when (pairState) {
                "started", "commissioning" -> "Parowanie w toku"
                "removing" -> "Usuwanie w toku"
                "success" -> "Zakończono"
                "failed" -> "Błąd"
                else -> "Brak aktywnej operacji"
            }
            KeyVal("Stan", stateLabel, when (pairState) {
                "failed" -> ErrorRed
                "success" -> AccentGreen
                "idle" -> TextMuted
                else -> WarnAmber
            })
            if (pairNode > 0) {
                KeyVal("Węzeł (Node)", pairNode.toString())
            }
            if (pairElapsed > 0) {
                KeyVal("Czas trwania", "${pairElapsed} s")
            }
            if (pairMessage.isNotBlank()) {
                KeyVal("Komunikat", pairMessage)
            }
            Spacer(Modifier.height(8.dp))
            OutlinedButton(onClick = onRefreshStatus, modifier = Modifier.fillMaxWidth()) {
                Text("Odśwież status parowania")
            }
        }
    }
}

@Composable
private fun DiagnosticsTab(
    diag: JSONObject?,
    settings: JSONObject?,
    client: GatewayApiClient,
    onLogout: () -> Unit
) {
    val scope = rememberCoroutineScope()
    var showRebootConfirm by remember { mutableStateOf(false) }
    var rebootFeedback by remember { mutableStateOf<String?>(null) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        if (diag != null) {
            val mem = diag.optJSONObject("memory")
            val flash = diag.optJSONObject("flash")
            val s3c6 = diag.optJSONObject("s3_c6")
            val cpu = diag.optJSONObject("cpu")
            val wifiHealth = diag.optJSONObject("wifi_health")
            val threadSettings = settings?.optJSONObject("thread")

            // 1. S3 - C6 Spinel & Thread Radio
            InfoCard("Połączenie S3 – C6 (Radio Thread)") {
                val ok = s3c6?.optBoolean("connected", false) == true
                val blocked = s3c6?.optBoolean("radio_access_blocked", false) == true
                val rawTransport = s3c6?.optString("transport", "-") ?: "-"
                val shortTransport = if (rawTransport.contains("2000000")) "UART1 2 Mb/s (8N1 USB)" else rawTransport
                KeyVal("Łącze Spinel", if (ok) "Połączone" else "Brak odpowiedzi", if (ok) AccentGreen else ErrorRed)
                KeyVal("Transport", shortTransport)
                KeyVal("Wersja RCP (C6)", s3c6?.optString("rcp_version", "-") ?: "-")
                if (s3c6 != null && !s3c6.isNull("last_success_age_s")) {
                    KeyVal("Ostatnia sonda Spinel", "${s3c6.optLong("last_success_age_s", 0)} s temu")
                }
                KeyVal("Błędy sondy Spinel", (s3c6?.optInt("probe_failures", 0) ?: 0).toString())
                if (s3c6 != null && s3c6.has("consecutive_probe_failures")) {
                    KeyVal("Kolejne błędy Spinel", s3c6.optInt("consecutive_probe_failures", 0).toString())
                }
                KeyVal("Kod ostatniej sondy", (s3c6?.optInt("last_result", 0) ?: 0).toString())
                KeyVal(
                    "Blokada dostępu CCA",
                    if (blocked) "TAK (kanał zajęty / blokada)" else "Nie (0 blokady)",
                    if (blocked) ErrorRed else AccentGreen
                )
                if (s3c6 != null && s3c6.has("consecutive_cca_blocked_windows")) {
                    val cw = s3c6.optInt("consecutive_cca_blocked_windows", 0)
                    val wd = s3c6.optBoolean("watchdog_alert", false)
                    KeyVal(
                        "Watchdog C6 (kolejne okna CCA)",
                        if (wd) "ALARM ($cw okien)" else "$cw okien (OK)",
                        if (wd) ErrorRed else AccentGreen
                    )
                }
                KeyVal(
                    "Ramki TX (suma / ostatnie okno)",
                    "${s3c6?.optLong("radio_tx_total", 0)} / +${s3c6?.optLong("radio_tx_since_last_probe", 0)}"
                )
                KeyVal(
                    "Błędy CCA (suma / ostatnie okno)",
                    "${s3c6?.optLong("radio_cca_total", 0)} / +${s3c6?.optLong("radio_cca_since_last_probe", 0)}"
                )
                KeyVal("Błędy RX (FCS + Sec)", (s3c6?.optLong("radio_rx_errors_total", 0) ?: 0).toString())
                if (threadSettings != null && threadSettings.has("channel")) {
                    KeyVal("Aktywny kanał Thread", threadSettings.optInt("channel").toString())
                }
            }

            // 2. System & CPU
            InfoCard("System i procesor ESP32-S3") {
                val gwName = settings?.optString("gateway_name", "") ?: ""
                if (gwName.isNotEmpty()) KeyVal("Nazwa bramki", gwName)
                KeyVal("Firmware", diag.optString("firmware", "-"))
                KeyVal("Czas działania", formatUptime(diag.optLong("uptime_s", 0L)))
                val rr = diag.optInt("reset_reason", -1)
                KeyVal("Kod ostatniego restartu", resetReasonLabel(rr))
                val crashGuard = diag.optJSONObject("crash_guard")
                if (crashGuard != null) {
                    val boots = crashGuard.optLong("boot_count", 0)
                    val panics = crashGuard.optLong("panic_count", 0)
                    val lastAbn = crashGuard.optInt("last_abnormal_reason", 0)
                    val lastUp = crashGuard.optLong("last_uptime_before_reset_s", 0)
                    KeyVal("Rozruchy / Paniki (RTC)", "$boots / $panics", if (panics == 0L) AccentGreen else WarnAmber)
                    if (lastAbn != 0) {
                        KeyVal("Ostatni kod awarii (RTC)", resetReasonLabel(lastAbn), WarnAmber)
                    }
                    if (lastUp > 0) {
                        KeyVal("Uptime przed restartem", formatUptime(lastUp))
                    }
                }
                if (cpu != null) {
                    val totalLoad = cpu.optDouble("load_pct", Double.NaN)
                    if (!totalLoad.isNaN()) {
                        KeyVal("Obciążenie CPU (średnie)", String.format(Locale.US, "%.1f%%", totalLoad))
                    }
                    val cores = cpu.optJSONArray("core_load_pct")
                    if (cores != null && cores.length() >= 2) {
                        KeyVal(
                            "Rdzeń 0 / Rdzeń 1",
                            String.format(Locale.US, "%.1f%% / %.1f%%", cores.optDouble(0, 0.0), cores.optDouble(1, 0.0))
                        )
                    }
                }
            }

            // 3. Memory (Internal DRAM & PSRAM)
            if (mem != null) {
                InfoCard("Pamięć RAM i PSRAM") {
                    val intFree = mem.optLong("internal_free", mem.optLong("internal_free_bytes", 0))
                    val intUsed = mem.optLong("internal_used", 0)
                    val intTotal = mem.optLong("internal_total", 0)
                    val intMin = mem.optLong("internal_minimum", mem.optLong("internal_min_free_bytes", 0))
                    val intLargest = mem.optLong("internal_largest", 0)

                    val psFree = mem.optLong("psram_free", mem.optLong("psram_free_bytes", 0))
                    val psUsed = mem.optLong("psram_used", 0)
                    val psTotal = mem.optLong("psram_total", 0)

                    KeyVal("Wolny RAM wewn.", formatBytes(intFree), if (intFree > 32768) AccentGreen else WarnAmber)
                    KeyVal("Użyty / Całkowity RAM wewn.", "${formatBytes(intUsed)} / ${formatBytes(intTotal)}")
                    KeyVal("Minimum wolnego RAM wewn.", formatBytes(intMin))
                    KeyVal("Największy blok RAM wewn.", formatBytes(intLargest))
                    Spacer(Modifier.height(4.dp))
                    KeyVal("Wolny PSRAM", formatBytes(psFree), AccentGreen)
                    KeyVal("Użyty / Całkowity PSRAM", "${formatBytes(psUsed)} / ${formatBytes(psTotal)}")
                }
            }

            // 4. Flash & OTA Partition
            if (flash != null) {
                InfoCard("Pamięć Flash S3 i partycja OTA") {
                    KeyVal("Aktywna partycja", flash.optString("running_partition", "-"), AccentGreen)
                    if (!flash.isNull("physical_bytes")) {
                        KeyVal("Rozmiar fizyczny Flash", formatBytes(flash.optLong("physical_bytes", 0)))
                    }
                    KeyVal("Rozmiar partycji aplikacji", formatBytes(flash.optLong("app_partition_bytes", 0)))
                    if (!flash.isNull("app_image_bytes")) {
                        KeyVal("Rozmiar obrazu firmware", formatBytes(flash.optLong("app_image_bytes", 0)))
                    }
                    if (!flash.isNull("app_partition_unused_bytes")) {
                        KeyVal("Wolne w partycji aplikacji", formatBytes(flash.optLong("app_partition_unused_bytes", 0)))
                    }
                    if (!flash.isNull("unassigned_bytes")) {
                        KeyVal("Nieprzypisane miejsce Flash", formatBytes(flash.optLong("unassigned_bytes", 0)))
                    }
                }
            }

            // 5. Wi-Fi Health
            if (wifiHealth != null) {
                InfoCard("Diagnostyka stabilności Wi-Fi") {
                    val probeActive = wifiHealth.optBoolean("gateway_probe_active", false)
                    val seenReply = wifiHealth.optBoolean("gateway_seen_reply", false)
                    val misses = wifiHealth.optInt("gateway_misses", 0)
                    KeyVal(
                        "Sonda ping routera (bramy)",
                        when {
                            !probeActive -> "Nieaktywna"
                            seenReply && misses == 0 -> "OK (odpowiada)"
                            seenReply -> "Ostrzeżenie (utracone: $misses)"
                            else -> "Brak odpowiedzi"
                        },
                        if (probeActive && seenReply && misses == 0) AccentGreen else WarnAmber
                    )
                    KeyVal("Rozłączenia / Ponowne łączenia", "${wifiHealth.optLong("disconnects", 0)} / ${wifiHealth.optLong("reconnects", 0)}")
                    KeyVal("Utraty IPv4 / Odzyskania (recovery)", "${wifiHealth.optLong("ip_losses", 0)} / ${wifiHealth.optLong("recoveries", 0)}")
                    val lastReason = wifiHealth.optInt("last_disconnect_reason", 0)
                    if (lastReason != 0) {
                        KeyVal("Ostatni kod rozłączenia Wi-Fi", lastReason.toString())
                    }
                }
            }
        } else {
            Text("Ładowanie pełnej diagnostyki...", color = TextMuted)
        }

        if (rebootFeedback != null) {
            Text(rebootFeedback!!, color = WarnAmber, style = MaterialTheme.typography.bodySmall)
        }

        OutlinedButton(
            onClick = { showRebootConfirm = true },
            modifier = Modifier.fillMaxWidth()
        ) {
            Icon(Icons.Default.PowerSettingsNew, contentDescription = null, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
            Text("Restartuj bramkę S3")
        }

        OutlinedButton(
            onClick = onLogout,
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("Wyloguj z tej bramki")
        }
    }

    if (showRebootConfirm) {
        AlertDialog(
            onDismissRequest = { showRebootConfirm = false },
            title = { Text("Restart bramki") },
            text = { Text("Czy na pewno chcesz zrestartować bramkę Matter2Supla (${client.ip})?") },
            confirmButton = {
                Button(
                    onClick = {
                        showRebootConfirm = false
                        scope.launch {
                            val r = client.rebootGateway()
                            rebootFeedback = if (r.isOk) "Wysłano polecenie restartu." else "Błąd restartu: ${r.error ?: r.code}"
                        }
                    }
                ) {
                    Text("Restartuj")
                }
            },
            dismissButton = {
                TextButton(onClick = { showRebootConfirm = false }) {
                    Text("Anuluj")
                }
            }
        )
    }
}

@Composable
private fun InfoCard(title: String, content: @Composable () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        border = BorderStroke(1.dp, CardBorder),
        shape = RoundedCornerShape(14.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                color = AccentGreen,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(Modifier.height(8.dp))
            content()
        }
    }
}

@Composable
private fun KeyVal(key: String, value: String, valueColor: Color = Color(0xFFF4F7FB)) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = key,
            style = MaterialTheme.typography.bodySmall,
            color = TextMuted,
            modifier = Modifier.weight(0.48f)
        )
        Spacer(Modifier.width(8.dp))
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            color = valueColor,
            fontWeight = FontWeight.Medium,
            textAlign = TextAlign.End,
            modifier = Modifier.weight(0.52f)
        )
    }
}

private fun formatUptime(seconds: Long): String {
    val d = seconds / 86400
    val h = (seconds % 86400) / 3600
    val m = (seconds % 3600) / 60
    val s = seconds % 60
    return if (d > 0) "${d}d ${h}h ${m}m" else if (h > 0) "${h}h ${m}m ${s}s" else "${m}m ${s}s"
}

private fun formatBytes(bytes: Long): String {
    if (bytes <= 0) return "0 B"
    return when {
        bytes >= 1024 * 1024 -> String.format(Locale.US, "%.2f MiB (%d KiB)", bytes / (1024.0 * 1024.0), bytes / 1024)
        bytes >= 1024 -> String.format(Locale.US, "%.1f KiB", bytes / 1024.0)
        else -> "$bytes B"
    }
}

private fun resetReasonLabel(code: Int): String = when (code) {
    1 -> "1 (POWERON)"
    3 -> "3 (SW / esp_restart)"
    4 -> "4 (PANIC)"
    5 -> "5 (INT_WDT)"
    6 -> "6 (TASK_WDT)"
    7 -> "7 (WDT)"
    9 -> "9 (BROWNOUT)"
    else -> code.toString()
}
