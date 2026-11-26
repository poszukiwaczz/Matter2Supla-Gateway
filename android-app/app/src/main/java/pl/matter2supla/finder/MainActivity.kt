package pl.matter2supla.finder

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Router
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

private val MatterDarkColors = darkColorScheme(
    primary = Color(0xFF55D47A),
    onPrimary = Color(0xFF062111),
    background = Color(0xFF081622),
    surface = Color(0xFF102131),
    surfaceVariant = Color(0xFF14283A),
    onBackground = Color(0xFFF4F7FB),
    onSurface = Color(0xFFF4F7FB),
    outline = Color(0xFF294052)
)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val storage = GatewayStorage(this)

        setContent {
            MaterialTheme(colorScheme = MatterDarkColors) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    Matter2SuplaApp(storage = storage)
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Matter2SuplaApp(storage: GatewayStorage) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var selectedGateway by remember { mutableStateOf<DiscoveredGateway?>(null) }
    var activeClient by remember { mutableStateOf<GatewayApiClient?>(null) }

    val gateways = remember { mutableStateListOf<DiscoveredGateway>() }
    var isScanning by remember { mutableStateOf(false) }
    var scannedCount by remember { mutableIntStateOf(0) }
    var totalCount by remember { mutableIntStateOf(0) }
    var localIp by remember { mutableStateOf<String?>(null) }
    var showManualDialog by remember { mutableStateOf(false) }
    var manualError by remember { mutableStateOf<String?>(null) }
    var scanJob by remember { mutableStateOf<Job?>(null) }

    fun startScan() {
        scanJob?.cancel()
        gateways.clear()
        localIp = GatewayDiscovery.getLocalWifiIpv4(context)
        isScanning = true
        scannedCount = 0
        totalCount = 0
        scanJob = scope.launch {
            try {
                GatewayDiscovery.scanNetwork(
                    context = context,
                    savedIps = storage.getSavedIps(),
                    onProgress = { done, total ->
                        scannedCount = done
                        totalCount = total
                    },
                    onFound = { gw ->
                        if (gateways.none { it.ip == gw.ip }) {
                            gateways.add(gw)
                        }
                    }
                )
            } finally {
                isScanning = false
            }
        }
    }

    LaunchedEffect(Unit) {
        startScan()
    }

    val gw = selectedGateway
    val client = activeClient
    if (gw != null && client != null) {
        NativeGatewayPanelScreen(
            gateway = gw,
            client = client,
            onBackToList = {
                activeClient = null
                selectedGateway = null
            },
            onLogout = {
                storage.clearCredential(gw.ip)
                activeClient = null
            }
        )
        return
    }

    if (gw != null) {
        GatewayLoginScreen(
            gateway = gw,
            initialCredential = storage.getSavedCredential(gw.ip) ?: "",
            onBack = { selectedGateway = null },
            onAuthenticated = { newClient, rememberPass ->
                if (rememberPass) {
                    storage.saveCredential(gw.ip, newClient.credential)
                } else {
                    storage.clearCredential(gw.ip)
                }
                activeClient = newClient
            }
        )
        return
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            imageVector = Icons.Default.Router,
                            contentDescription = null,
                            tint = Color(0xFF55D47A),
                            modifier = Modifier.size(24.dp)
                        )
                        Spacer(Modifier.width(10.dp))
                        Text("Matter2Supla", fontWeight = FontWeight.SemiBold)
                    }
                },
                actions = {
                    IconButton(onClick = { showManualDialog = true }) {
                        Icon(Icons.Default.Add, contentDescription = "Dodaj IP ręcznie")
                    }
                    IconButton(onClick = { startScan() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Skanuj sieć")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = Color(0xFF081622),
                    titleContentColor = Color(0xFFF4F7FB),
                    actionIconContentColor = Color(0xFF55D47A)
                )
            )
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 16.dp, vertical = 12.dp)
        ) {
            Card(
                colors = CardDefaults.cardColors(containerColor = Color(0xFF102131)),
                border = BorderStroke(1.dp, Color(0xFF294052)),
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(
                    modifier = Modifier.padding(14.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        imageVector = Icons.Default.Wifi,
                        contentDescription = null,
                        tint = Color(0xFF55D47A)
                    )
                    Spacer(Modifier.width(12.dp))
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            text = if (localIp != null) "Sieć lokalna: $localIp" else "Brak adresu Wi-Fi IPv4",
                            style = MaterialTheme.typography.bodyMedium,
                            fontWeight = FontWeight.Medium
                        )
                        Text(
                            text = if (isScanning) {
                                "Skanowanie sieci lokalnej ($scannedCount/$totalCount)..."
                            } else {
                                "Znaleziono bramek: ${gateways.size}"
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = Color(0xFF9EB3C2)
                        )
                    }
                }
                if (isScanning) {
                    val fraction = if (totalCount > 0) scannedCount.toFloat() / totalCount else 0f
                    LinearProgressIndicator(
                        progress = { fraction },
                        modifier = Modifier.fillMaxWidth(),
                        color = Color(0xFF55D47A),
                        trackColor = Color(0xFF14283A)
                    )
                }
            }

            Spacer(Modifier.height(16.dp))

            if (gateways.isEmpty() && !isScanning) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(1f),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            text = "Nie znaleziono bramki Matter2Supla w sieci lokalnej.",
                            style = MaterialTheme.typography.bodyLarge
                        )
                        Spacer(Modifier.height(8.dp))
                        Text(
                            text = "Upewnij się, że telefon jest w tej samej sieci Wi-Fi co bramka (lub połączony z Matter-GW).",
                            style = MaterialTheme.typography.bodySmall,
                            color = Color(0xFF9EB3C2)
                        )
                        Spacer(Modifier.height(16.dp))
                        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            Button(
                                onClick = { startScan() },
                                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF55D47A))
                            ) {
                                Text("Skanuj ponownie", color = Color(0xFF062111))
                            }
                            OutlinedButton(onClick = { showManualDialog = true }) {
                                Text("Wpisz IP")
                            }
                        }
                    }
                }
            } else {
                LazyColumn(
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                    modifier = Modifier.weight(1f)
                ) {
                    items(gateways, key = { it.ip }) { item ->
                        GatewayCard(
                            gateway = item,
                            onOpen = {
                                storage.saveIp(item.ip)
                                storage.saveTrustedFingerprint(item.ip, item.certSha256)
                                selectedGateway = item
                            }
                        )
                    }
                }
            }
        }
    }

    if (showManualDialog) {
        var ipInput by remember { mutableStateOf("") }
        var probing by remember { mutableStateOf(false) }

        AlertDialog(
            onDismissRequest = {
                if (!probing) {
                    showManualDialog = false
                    manualError = null
                }
            },
            title = { Text("Połącz po adresie IP") },
            text = {
                Column {
                    OutlinedTextField(
                        value = ipInput,
                        onValueChange = { ipInput = it; manualError = null },
                        label = { Text("Adres IP bramki (np. 192.168.1.191)") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                    if (manualError != null) {
                        Spacer(Modifier.height(8.dp))
                        Text(
                            text = manualError!!,
                            color = Color(0xFFFF6B6B),
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
            },
            confirmButton = {
                Button(
                    enabled = !probing && ipInput.isNotBlank(),
                    onClick = {
                        probing = true
                        manualError = null
                        scope.launch {
                            val found = GatewayDiscovery.probeSingleIp(ipInput)
                            probing = false
                            if (found != null) {
                                if (gateways.none { it.ip == found.ip }) {
                                    gateways.add(0, found)
                                }
                                storage.saveIp(found.ip)
                                storage.saveTrustedFingerprint(found.ip, found.certSha256)
                                showManualDialog = false
                                selectedGateway = found
                            } else {
                                manualError = "Nie wykryto bramki Matter2Supla pod tym IP (port 443)."
                            }
                        }
                    }
                ) {
                    Text(if (probing) "Sprawdzanie..." else "Połącz")
                }
            },
            dismissButton = {
                TextButton(
                    enabled = !probing,
                    onClick = {
                        showManualDialog = false
                        manualError = null
                    }
                ) {
                    Text("Anuluj")
                }
            }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GatewayLoginScreen(
    gateway: DiscoveredGateway,
    initialCredential: String,
    onBack: () -> Unit,
    onAuthenticated: (GatewayApiClient, Boolean) -> Unit
) {
    val scope = rememberCoroutineScope()
    var credential by remember { mutableStateOf(initialCredential) }
    var rememberPass by remember { mutableStateOf(true) }
    var checking by remember { mutableStateOf(false) }
    var errorMsg by remember { mutableStateOf<String?>(null) }

    fun tryLogin(cred: String) {
        if (cred.isBlank()) return
        scope.launch {
            checking = true
            errorMsg = null
            val client = GatewayApiClient(gateway.ip, gateway.certSha256, cred.trim())
            val res = client.getStatus()
            checking = false
            if (res.isOk) {
                onAuthenticated(client, rememberPass)
            } else {
                errorMsg = res.json?.optString("message")?.ifEmpty { null }
                    ?: if (res.code == 401) "Nieprawidłowe hasło lub token panelu."
                    else res.error ?: "Błąd połączenia (HTTP ${res.code})."
            }
        }
    }

    LaunchedEffect(gateway.ip) {
        if (initialCredential.isNotBlank()) {
            tryLogin(initialCredential)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Logowanie — ${gateway.name}") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Powrót")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = Color(0xFF081622),
                    titleContentColor = Color(0xFFF4F7FB),
                    navigationIconContentColor = Color(0xFFF4F7FB)
                )
            )
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(20.dp),
            verticalArrangement = Arrangement.Center
        ) {
            Card(
                colors = CardDefaults.cardColors(containerColor = Color(0xFF102131)),
                border = BorderStroke(1.dp, Color(0xFF294052)),
                shape = RoundedCornerShape(16.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(modifier = Modifier.padding(20.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Default.Lock, contentDescription = null, tint = Color(0xFF55D47A))
                        Spacer(Modifier.width(10.dp))
                        Column {
                            Text(gateway.name, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                            Text("https://${gateway.ip}", style = MaterialTheme.typography.bodySmall, color = Color(0xFF9EB3C2))
                        }
                    }

                    Spacer(Modifier.height(16.dp))

                    OutlinedTextField(
                        value = credential,
                        onValueChange = { credential = it; errorMsg = null },
                        label = { Text("Hasło panelu lub token") },
                        visualTransformation = PasswordVisualTransformation(),
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(Modifier.height(8.dp))

                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = rememberPass, onCheckedChange = { rememberPass = it })
                        Text("Zapamiętaj hasło na tym telefonie", style = MaterialTheme.typography.bodySmall)
                    }

                    if (errorMsg != null) {
                        Spacer(Modifier.height(8.dp))
                        Text(errorMsg!!, color = Color(0xFFFF6B6B), style = MaterialTheme.typography.bodySmall)
                    }

                    Spacer(Modifier.height(16.dp))

                    Button(
                        enabled = !checking && credential.isNotBlank(),
                        onClick = { tryLogin(credential) },
                        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF55D47A)),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(
                            text = if (checking) "Łączenie..." else "Zaloguj się",
                            color = Color(0xFF062111),
                            fontWeight = FontWeight.SemiBold
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun GatewayCard(
    gateway: DiscoveredGateway,
    onOpen: () -> Unit
) {
    Card(
        colors = CardDefaults.cardColors(containerColor = Color(0xFF102131)),
        border = BorderStroke(1.dp, Color(0xFF294052)),
        shape = RoundedCornerShape(14.dp),
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onOpen)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = gateway.name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold
                    )
                    Text(
                        text = "https://${gateway.ip}",
                        style = MaterialTheme.typography.bodyMedium,
                        color = Color(0xFF55D47A)
                    )
                }
                Box(
                    modifier = Modifier
                        .background(Color(0xFF14283A), RoundedCornerShape(8.dp))
                        .padding(horizontal = 10.dp, vertical = 4.dp)
                ) {
                    Text(
                        text = gateway.source,
                        style = MaterialTheme.typography.labelSmall,
                        color = Color(0xFF9EB3C2)
                    )
                }
            }

            Spacer(Modifier.height(8.dp))

            if (!gateway.version.isNullOrBlank()) {
                Text(
                    text = "Firmware: ${gateway.version}",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color(0xFF9EB3C2)
                )
            }
            if (gateway.setupRequired) {
                Text(
                    text = "Wymaga pierwszego ustawienia hasła panelu",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color(0xFFFFD166)
                )
            }

            Spacer(Modifier.height(6.dp))

            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    imageVector = Icons.Default.Security,
                    contentDescription = null,
                    tint = Color(0xFF9EB3C2),
                    modifier = Modifier.size(14.dp)
                )
                Spacer(Modifier.width(6.dp))
                Text(
                    text = "SHA-256: ${gateway.certSha256.take(23)}...",
                    style = MaterialTheme.typography.labelSmall,
                    color = Color(0xFF9EB3C2)
                )
            }

            Spacer(Modifier.height(12.dp))

            Button(
                onClick = onOpen,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF55D47A))
            ) {
                Text("Połącz z bramką", color = Color(0xFF062111), fontWeight = FontWeight.SemiBold)
            }
        }
    }
}
