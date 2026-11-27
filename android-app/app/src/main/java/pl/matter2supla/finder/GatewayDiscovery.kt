package pl.matter2supla.finder

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.X509TrustManager

data class DiscoveredGateway(
    val ip: String,
    val name: String,
    val version: String?,
    val certSha256: String,
    val setupRequired: Boolean,
    val source: String
)

object GatewayDiscovery {

    fun getLocalWifiIpv4(context: Context): String? {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
        val activeNetwork = cm?.activeNetwork
        val caps = cm?.getNetworkCapabilities(activeNetwork)
        if (caps != null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
            val linkProps = cm.getLinkProperties(activeNetwork)
            linkProps?.linkAddresses?.forEach { linkAddr ->
                val addr = linkAddr.address
                if (addr is Inet4Address && !addr.isLoopbackAddress) {
                    return addr.hostAddress
                }
            }
        }
        return try {
            NetworkInterface.getNetworkInterfaces()?.toList()
                ?.filter { it.isUp && !it.isLoopback }
                ?.flatMap { it.inetAddresses.toList() }
                ?.firstOrNull { it is Inet4Address && !it.isLoopbackAddress && it.isSiteLocalAddress }
                ?.hostAddress
        } catch (_: Exception) {
            null
        }
    }

    suspend fun scanNetwork(
        context: Context,
        savedIps: List<String>,
        onProgress: (scanned: Int, total: Int) -> Unit,
        onFound: (DiscoveredGateway) -> Unit
    ) = withContext(Dispatchers.IO) {
        val localIp = getLocalWifiIpv4(context)
        val candidates = LinkedHashSet<String>()

        // 1. Saved IPs first for instant hit
        savedIps.forEach { candidates.add(it) }
        // 2. AP provisioning IP
        candidates.add("192.168.4.1")

        // 3. Quick mDNS/NSD pass (1.2s) for _https._tcp and _matter._tcp
        val mdnsIps = discoverViaNsd(context, 1200L)
        mdnsIps.forEach { candidates.add(it) }

        // 4. Current Wi-Fi /24 subnet
        if (localIp != null) {
            val parts = localIp.split(".")
            if (parts.size == 4) {
                val prefix = "${parts[0]}.${parts[1]}.${parts[2]}"
                for (host in 1..254) {
                    val ip = "$prefix.$host"
                    if (ip != localIp) {
                        candidates.add(ip)
                    }
                }
            }
        }

        val total = candidates.size
        val scanned = AtomicInteger(0)
        val foundIps = ConcurrentHashMap.newKeySet<String>()
        val semaphore = Semaphore(32)

        coroutineScope {
            candidates.map { ip ->
                async {
                    semaphore.withPermit {
                        val source = when {
                            ip in savedIps -> "Zapisana"
                            ip == "192.168.4.1" -> "Tryb AP"
                            ip in mdnsIps -> "mDNS"
                            else -> "Skan LAN"
                        }
                        val gw = probeGateway(ip, source)
                        if (gw != null && foundIps.add(gw.ip)) {
                            withContext(Dispatchers.Main) {
                                onFound(gw)
                            }
                        }
                        val done = scanned.incrementAndGet()
                        if (done % 8 == 0 || done == total) {
                            withContext(Dispatchers.Main) {
                                onProgress(done, total)
                            }
                        }
                    }
                }
            }.awaitAll()
        }
    }

    suspend fun probeSingleIp(ip: String): DiscoveredGateway? = withContext(Dispatchers.IO) {
        val groups = ip.trim().split('.')
        if (groups.size != 4) return@withContext null
        val octets = groups.map {
            it.takeIf { part -> part.length in 1..3 && part.all { digit -> digit in '0'..'9' } }
                ?.toIntOrNull()?.takeIf { octet -> octet in 0..255 } ?: return@withContext null
        }
        probeGateway(octets.joinToString("."), "Ręczny IP", connectTimeoutMs = 1500)
    }

    private suspend fun discoverViaNsd(context: Context, durationMs: Long): Set<String> {
        val discovered = ConcurrentHashMap.newKeySet<String>()
        val nsdManager = context.getSystemService(Context.NSD_SERVICE) as? NsdManager ?: return emptySet()
        val serviceTypes = listOf("_matter2supla._tcp.", "_https._tcp.", "_matter._tcp.")
        val listeners = mutableListOf<NsdManager.DiscoveryListener>()

        for (type in serviceTypes) {
            val listener = object : NsdManager.DiscoveryListener {
                override fun onDiscoveryStarted(regType: String) {}
                override fun onServiceFound(service: NsdServiceInfo) {
                    try {
                        @Suppress("DEPRECATION")
                        nsdManager.resolveService(service, object : NsdManager.ResolveListener {
                            override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {}
                            override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                                @Suppress("DEPRECATION")
                                val host = serviceInfo.host
                                if (host is Inet4Address && host.hostAddress != null) {
                                    discovered.add(host.hostAddress!!)
                                }
                            }
                        })
                    } catch (_: Exception) {
                    }
                }

                override fun onServiceLost(service: NsdServiceInfo) {}
                override fun onDiscoveryStopped(serviceType: String) {}
                override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {}
                override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            }
            try {
                nsdManager.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, listener)
                listeners.add(listener)
            } catch (_: Exception) {
            }
        }

        try {
            delay(durationMs)
        } finally {
            for (listener in listeners) {
                try {
                    nsdManager.stopServiceDiscovery(listener)
                } catch (_: Exception) {
                }
            }
        }
        return discovered
    }

    private fun probeGateway(
        ip: String,
        source: String,
        connectTimeoutMs: Int = 400
    ): DiscoveredGateway? {
        // Step 1: Fast TCP connect to port 443
        try {
            Socket().use { tcp ->
                tcp.connect(InetSocketAddress(ip, 443), connectTimeoutMs)
            }
        } catch (_: Exception) {
            return null
        }

        // Step 2: TLS handshake + cert inspection + GET /api/status
        var capturedCert: X509Certificate? = null
        val trustManager = object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                capturedCert = chain?.firstOrNull()
            }
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }

        return try {
            val sslContext = SSLContext.getInstance("TLS")
            sslContext.init(null, arrayOf(trustManager), SecureRandom())
            val socket = sslContext.socketFactory.createSocket() as SSLSocket
            socket.use { ssl ->
                ssl.connect(InetSocketAddress(ip, 443), 1200)
                ssl.soTimeout = 1500
                ssl.startHandshake()

                val cert = capturedCert ?: return null
                val subjectDn = cert.subjectX500Principal.name
                val certMatchesMatter2Supla =
                    subjectDn.contains("matter2supla", ignoreCase = true)
                val sha256 = sha256Hex(cert.encoded)

                // Probe /api/status (allowed Host header = IP)
                val req = "GET /api/status HTTP/1.1\r\nHost: $ip\r\nConnection: close\r\n\r\n"
                ssl.outputStream.write(req.toByteArray(Charsets.UTF_8))
                ssl.outputStream.flush()

                val reader = BufferedReader(InputStreamReader(ssl.inputStream, Charsets.UTF_8))
                val response = buildString {
                    var line: String?
                    var totalChars = 0
                    while (reader.readLine().also { line = it } != null && totalChars < 8192) {
                        append(line).append('\n')
                        totalChars += (line?.length ?: 0)
                    }
                }

                val body = response.substringAfter("\n\n", "")
                var gwName = "Matter2Supla"
                var version: String? = null
                var setupRequired = false
                var apiConfirmed = false

                if (body.trim().startsWith("{")) {
                    try {
                        val json = JSONObject(body.trim())
                        if (json.has("setup_required")) {
                            setupRequired = json.optBoolean("setup_required", false)
                            apiConfirmed = true
                        }
                        val sys = json.optJSONObject("system")
                        if (sys != null) {
                            val n = sys.optString("gateway_name", "")
                            if (n.isNotEmpty()) gwName = n
                            val v = sys.optString("version", "")
                            if (v.isNotEmpty()) version = v
                            apiConfirmed = true
                        }
                        if (json.has("supla") || json.has("thread")) {
                            apiConfirmed = true
                        }
                        val msg = json.optString("message", "")
                        if (msg.contains("Wymagane haslo lub token panelu", ignoreCase = true)) {
                            apiConfirmed = true
                        }
                    } catch (_: Exception) {
                    }
                }

                if (certMatchesMatter2Supla || apiConfirmed) {
                    DiscoveredGateway(
                        ip = ip,
                        name = gwName,
                        version = version,
                        certSha256 = sha256,
                        setupRequired = setupRequired,
                        source = source
                    )
                } else {
                    null
                }
            }
        } catch (_: Exception) {
            null
        }
    }

    fun sha256Hex(bytes: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(bytes)
        return digest.joinToString(":") { "%02X".format(it) }
    }
}
