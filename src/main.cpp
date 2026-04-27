#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Esp.h>

// Config vars for hardcoded data
static const char *WIFI_SSID = "";
static const char *WIFI_PASSWORD = "";
static const char *DEVICE_HOSTNAME = "sixeyes-node-01";

static const bool EXPOSE_NETWORK_DETAILS = false; // set to false in production

static const char *PROJECT_NAME = "SixEyes";
// Versioning for production-use cases
static const char *FIRMWARE_VERSION = "1.0.0";

// config var for dynamic ip hardcoding, in case
static const bool USE_STATIC_IP = false;
static const IPAddress STATIC_IP(0, 0, 0, 0);
static const IPAddress STATIC_GATEWAY(0, 0, 0, 0);
static const IPAddress STATIC_SUBNET(0, 0, 0, 0);
static const IPAddress STATIC_DNS1(0, 0, 0, 0);
static const IPAddress STATIC_DNS2(0, 0, 0, 0);

static const uint16_t HTTP_PORT = 80;
static const unsigned long TELEMETRY_SAMPLE_INTERVAL_MS = 3000;
static const unsigned long RECONNECT_INTERVAL_MS = 5000;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

static const uint32_t HEAP_WARN_THRESHOLD_BYTES = 30000;
static const long RSSI_WEAK_THRESHOLD_DBM = -75;
static const unsigned long RECONNECT_RATE_WINDOW_MS = 60000;

// Sampling for metrics
static const size_t MAX_EVENTS = 20;
static const size_t MAX_RSSI_SAMPLES = 30;
static const size_t MAX_RECONNECT_TIMESTAMPS = 10;

WebServer server(HTTP_PORT);

// Event Schema level
struct EventEntry
{
    unsigned long tsMs;
    String level;
    String type;
    String message;
};

// WIFI Telemetry Schema
struct WifiTelemetryState
{
    bool connected;
    String statusText;
    String ssid;
    String hostname;
    IPAddress localIP;
    IPAddress gatewayIP;
    IPAddress subnetMask;
    IPAddress dns1;
    IPAddress dns2;
    long rssi;
    unsigned long reconnectCount;
    int lastDisconnectReason;
    unsigned long lastSampleMs;
};

// System Telemetry Schema
struct SystemTelemetryState
{
    unsigned long uptimeMs;
    uint32_t freeHeap;
    String sdkVersion;
    String coreVersion;
};

WifiTelemetryState wifiState;
SystemTelemetryState systemState;

EventEntry eventLog[MAX_EVENTS];
size_t eventCount = 0;
size_t eventWriteIndex = 0;

long rssiHistory[MAX_RSSI_SAMPLES];
size_t rssiSampleCount = 0;
size_t rssiWriteIndex = 0;

unsigned long reconnectTimestamps[MAX_RECONNECT_TIMESTAMPS];
size_t reconnectTimestampCount = 0;
size_t reconnectTimestampWriteIndex = 0;

unsigned long lastTelemetrySampleMs = 0;
unsigned long lastReconnectAttemptMs = 0;

// == utility helpers

String jsonEscape(const String &input)
{
    String out;
    out.reserve(input.length() + 8);

    for (size_t i = 0; i < input.length(); i++)
    {
        char c = input[i];
        switch (c)
        {
        case '\"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

String ipToString(const IPAddress &ip)
{
    return ip.toString();
}

bool isZeroIP(const IPAddress &ip)
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

String boolJson(bool value)
{
    return value ? "true" : "false";
}

// Redaction handling
String redactableString(const String &value)
{
    return EXPOSE_NETWORK_DETAILS ? value : "redacted";
}

String redactableIP(const IPAddress &ip)
{
    return EXPOSE_NETWORK_DETAILS ? ip.toString() : "redacted";
}

String formatUptime(unsigned long ms)
{
    unsigned long totalSeconds = ms / 1000UL;
    unsigned long days = totalSeconds / 86400UL;
    totalSeconds %= 86400UL;
    unsigned long hours = totalSeconds / 3600UL;
    totalSeconds %= 3600UL;
    unsigned long minutes = totalSeconds / 60UL;
    unsigned long seconds = totalSeconds % 60UL;

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lu d %02lu:%02lu:%02lu",
             days, hours, minutes, seconds);
    return String(buffer);
}

int clampInt(int value, int minValue, int maxValue)
{
    if (value < minValue)
        return minValue;
    if (value > maxValue)
        return maxValue;
    return value;
}

String wifiStatusToString(wl_status_t status)
{
    switch (status)
    {
    case WL_IDLE_STATUS:
        return "IDLE";
    case WL_NO_SSID_AVAIL:
        return "NO_SSID_AVAILABLE";
    case WL_SCAN_COMPLETED:
        return "SCAN_COMPLETED";
    case WL_CONNECTED:
        return "CONNECTED";
    case WL_CONNECT_FAILED:
        return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
        return "CONNECTION_LOST";
    case WL_DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN";
    }
}

String disconnectReasonToString(int reason)
{
    switch (reason)
    {
    case -1:
        return "NONE_RECORDED";
    case 1:
        return "UNSPECIFIED";
    case 2:
        return "AUTH_EXPIRE";
    case 3:
        return "AUTH_LEAVE";
    case 4:
        return "ASSOC_EXPIRE";
    case 5:
        return "ASSOC_TOOMANY";
    case 6:
        return "NOT_AUTHED";
    case 7:
        return "NOT_ASSOCED";
    case 8:
        return "ASSOC_LEAVE";
    case 15:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17:
        return "IE_IN_4WAY_DIFFERS";
    case 23:
        return "802_1X_AUTH_FAILED";
    case 200:
        return "BEACON_TIMEOUT";
    case 201:
        return "NO_AP_FOUND";
    case 202:
        return "AUTH_FAIL";
    case 203:
        return "ASSOC_FAIL";
    case 204:
        return "HANDSHAKE_TIMEOUT";
    default:
        return "UNKNOWN_REASON";
    }
}

// == event logging

void addEvent(const String &level, const String &type, const String &message)
{
    eventLog[eventWriteIndex].tsMs = millis();
    eventLog[eventWriteIndex].level = level;
    eventLog[eventWriteIndex].type = type;
    eventLog[eventWriteIndex].message = message;

    eventWriteIndex = (eventWriteIndex + 1) % MAX_EVENTS;
    if (eventCount < MAX_EVENTS)
    {
        eventCount++;
    }

    Serial.print("[");
    Serial.print(level);
    Serial.print("] ");
    Serial.print(type);
    Serial.print(" - ");
    Serial.println(message);
}

void addReconnectTimestamp()
{
    reconnectTimestamps[reconnectTimestampWriteIndex] = millis();
    reconnectTimestampWriteIndex = (reconnectTimestampWriteIndex + 1) % MAX_RECONNECT_TIMESTAMPS;

    if (reconnectTimestampCount < MAX_RECONNECT_TIMESTAMPS)
    {
        reconnectTimestampCount++;
    }
}

unsigned long reconnectsInWindow(unsigned long windowMs)
{
    unsigned long now = millis();
    unsigned long count = 0;

    for (size_t i = 0; i < reconnectTimestampCount; i++)
    {
        if (now - reconnectTimestamps[i] <= windowMs)
        {
            count++;
        }
    }

    return count;
}

void addRssiSample(long value)
{
    rssiHistory[rssiWriteIndex] = value;
    rssiWriteIndex = (rssiWriteIndex + 1) % MAX_RSSI_SAMPLES;

    if (rssiSampleCount < MAX_RSSI_SAMPLES)
    {
        rssiSampleCount++;
    }
}

// == health model

String currentHealthStatus()
{
    bool ipAssigned = !isZeroIP(wifiState.localIP);
    bool heapOk = systemState.freeHeap >= HEAP_WARN_THRESHOLD_BYTES;
    bool weakSignal = wifiState.connected && wifiState.rssi <= RSSI_WEAK_THRESHOLD_DBM;
    bool reconnectStorm = reconnectsInWindow(RECONNECT_RATE_WINDOW_MS) >= 3;

    if (!wifiState.connected || !ipAssigned)
    {
        return "down";
    }

    if (!heapOk || weakSignal || reconnectStorm)
    {
        return "degraded";
    }

    return "ok";
}

String healthReasonText()
{
    bool ipAssigned = !isZeroIP(wifiState.localIP);
    bool heapOk = systemState.freeHeap >= HEAP_WARN_THRESHOLD_BYTES;
    bool weakSignal = wifiState.connected && wifiState.rssi <= RSSI_WEAK_THRESHOLD_DBM;
    bool reconnectStorm = reconnectsInWindow(RECONNECT_RATE_WINDOW_MS) >= 3;

    if (!wifiState.connected)
        return "wifi_disconnected";
    if (!ipAssigned)
        return "ip_not_assigned";
    if (!heapOk)
        return "low_heap";
    if (weakSignal)
        return "weak_signal";
    if (reconnectStorm)
        return "reconnect_storm";

    return "healthy";
}

// == wi-fi funcs

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event)
    {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        addEvent("INFO", "wifi_connected", "Wi-Fi station connected to access point");
        break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        addEvent("INFO", "wifi_got_ip", "Wi-Fi IP address assigned");
        break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        wifiState.reconnectCount++;
        wifiState.lastDisconnectReason = info.wifi_sta_disconnected.reason;
        addReconnectTimestamp();

        addEvent(
            "WARN",
            "wifi_disconnected",
            "Reason code=" + String(wifiState.lastDisconnectReason) +
                " (" + disconnectReasonToString(wifiState.lastDisconnectReason) + ")");
        break;

    default:
        break;
    }
}

bool connectToWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(DEVICE_HOSTNAME);

    if (USE_STATIC_IP)
    {
        if (!WiFi.config(STATIC_IP, STATIC_GATEWAY, STATIC_SUBNET, STATIC_DNS1, STATIC_DNS2))
        {
            addEvent("ERROR", "static_ip_failed", "Failed to apply static IP configuration");
            return false;
        }
        addEvent("INFO", "static_ip_enabled", "Static IP configuration applied");
    }

    addEvent("INFO", "wifi_connect_start", "Connecting to configured Wi-Fi network");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        addEvent("INFO", "wifi_connect_success", "Wi-Fi connection established");

        Serial.print("Dashboard URL: http://");
        Serial.println(WiFi.localIP());

        return true;
    }

    addEvent("ERROR", "wifi_connect_timeout", "Initial Wi-Fi connection timed out");
    return false;
}

void maybeReconnectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return;
    }

    unsigned long now = millis();
    if (now - lastReconnectAttemptMs < RECONNECT_INTERVAL_MS)
    {
        return;
    }

    lastReconnectAttemptMs = now;
    addEvent("WARN", "wifi_reconnect_attempt", "Attempting Wi-Fi reconnect");
    WiFi.disconnect(false, false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void sampleTelemetry()
{
    wifiState.connected = (WiFi.status() == WL_CONNECTED);
    wifiState.statusText = wifiStatusToString(WiFi.status());
    wifiState.ssid = EXPOSE_NETWORK_DETAILS ? WiFi.SSID() : "redacted";
    wifiState.hostname = WiFi.getHostname() ? String(WiFi.getHostname()) : String("N/A");
    wifiState.localIP = WiFi.localIP();
    wifiState.gatewayIP = WiFi.gatewayIP();
    wifiState.subnetMask = WiFi.subnetMask();
    wifiState.dns1 = WiFi.dnsIP(0);
    wifiState.dns2 = WiFi.dnsIP(1);
    wifiState.rssi = wifiState.connected ? WiFi.RSSI() : -127; // avoid showing 0 dBm when disconnected
    wifiState.lastSampleMs = millis();

    systemState.uptimeMs = millis();
    systemState.freeHeap = ESP.getFreeHeap();
    systemState.sdkVersion = ESP.getSdkVersion();
    systemState.coreVersion = ESP.getCoreVersion();

    if (wifiState.connected)
    {
        addRssiSample(wifiState.rssi);
    }
}

// == json builders

String buildInfoJson()
{
    String json = "{";
    json += "\"project\":\"" + jsonEscape(PROJECT_NAME) + "\",";
    json += "\"firmware_version\":\"" + jsonEscape(FIRMWARE_VERSION) + "\",";
    json += "\"hostname\":\"" + jsonEscape(wifiState.hostname) + "\",";
    json += "\"mac_address\":\"" + redactableString(WiFi.macAddress()) + "\",";
    json += "\"sdk_version\":\"" + jsonEscape(systemState.sdkVersion) + "\",";
    json += "\"core_version\":\"" + jsonEscape(systemState.coreVersion) + "\",";
    json += "\"static_ip_enabled\":" + boolJson(USE_STATIC_IP);
    json += "}";
    return json;
}

String buildHealthJson()
{
    bool ipAssigned = !isZeroIP(wifiState.localIP);
    bool heapOk = systemState.freeHeap >= HEAP_WARN_THRESHOLD_BYTES;
    bool weakSignal = wifiState.connected && wifiState.rssi <= RSSI_WEAK_THRESHOLD_DBM;
    unsigned long reconnectRate = reconnectsInWindow(RECONNECT_RATE_WINDOW_MS);

    String json = "{";
    json += "\"status\":\"" + currentHealthStatus() + "\",";
    json += "\"reason\":\"" + healthReasonText() + "\",";
    json += "\"checks\":{";
    json += "\"wifi_connected\":" + boolJson(wifiState.connected) + ",";
    json += "\"ip_assigned\":" + boolJson(ipAssigned) + ",";
    json += "\"heap_ok\":" + boolJson(heapOk) + ",";
    json += "\"weak_signal\":" + boolJson(weakSignal) + ",";
    json += "\"reconnect_storm\":" + boolJson(reconnectRate >= 3);
    json += "},";
    json += "\"metrics\":{";
    json += "\"uptime_ms\":" + String(systemState.uptimeMs) + ",";
    json += "\"free_heap\":" + String(systemState.freeHeap) + ",";
    json += "\"rssi_dbm\":" + String(wifiState.rssi) + ",";
    json += "\"reconnect_count\":" + String(wifiState.reconnectCount) + ",";
    json += "\"reconnects_last_60s\":" + String(reconnectRate);
    json += "}";
    json += "}";

    return json;
}

String buildEventsJson()
{
    String json = "{";
    json += "\"events\":[";

    for (size_t i = 0; i < eventCount; i++)
    {
        size_t index = (eventWriteIndex + MAX_EVENTS - eventCount + i) % MAX_EVENTS;
        json += "{";
        json += "\"ts_ms\":" + String(eventLog[index].tsMs) + ",";
        json += "\"level\":\"" + jsonEscape(eventLog[index].level) + "\",";
        json += "\"type\":\"" + jsonEscape(eventLog[index].type) + "\",";
        json += "\"message\":\"" + jsonEscape(eventLog[index].message) + "\"";
        json += "}";

        if (i + 1 < eventCount)
            json += ",";
    }

    json += "]}";
    return json;
}

String buildMetricsJson()
{
    String json = "{";
    json += "\"schema_version\":\"1.0\",";
    json += "\"device_id\":\"" + jsonEscape(wifiState.hostname) + "\",";
    json += "\"timestamp_ms\":" + String(millis()) + ",";
    json += "\"health\":{";
    json += "\"status\":\"" + currentHealthStatus() + "\",";
    json += "\"reason\":\"" + healthReasonText() + "\"";
    json += "},";

    json += "\"wifi\":{";
    json += "\"connected\":" + boolJson(wifiState.connected) + ",";
    json += "\"status\":\"" + jsonEscape(wifiState.statusText) + "\",";
    json += "\"ssid\":\"" + jsonEscape(redactableString(wifiState.ssid)) + "\",";
    json += "\"ip\":\"" + redactableIP(wifiState.localIP) + "\",";
    json += "\"gateway\":\"" + redactableIP(wifiState.gatewayIP) + "\",";
    json += "\"subnet\":\"" + redactableIP(wifiState.subnetMask) + "\",";
    json += "\"dns1\":\"" + redactableIP(wifiState.dns1) + "\",";
    json += "\"dns2\":\"" + redactableIP(wifiState.dns2) + "\",";
    json += "\"rssi_dbm\":" + String(wifiState.rssi) + ",";
    json += "\"reconnect_count\":" + String(wifiState.reconnectCount) + ",";
    json += "\"reconnects_last_60s\":" + String(reconnectsInWindow(RECONNECT_RATE_WINDOW_MS)) + ",";
    json += "\"last_disconnect_reason_code\":" + String(wifiState.lastDisconnectReason) + ",";
    json += "\"last_disconnect_reason_text\":\"" + jsonEscape(disconnectReasonToString(wifiState.lastDisconnectReason)) + "\"";
    json += "},";

    json += "\"system\":{";
    json += "\"uptime_ms\":" + String(systemState.uptimeMs) + ",";
    json += "\"uptime_human\":\"" + formatUptime(systemState.uptimeMs) + "\",";
    json += "\"free_heap\":" + String(systemState.freeHeap) + ",";
    json += "\"sdk_version\":\"" + jsonEscape(systemState.sdkVersion) + "\",";
    json += "\"core_version\":\"" + jsonEscape(systemState.coreVersion) + "\"";
    json += "},";

    json += "\"rssi_history\":[";
    for (size_t i = 0; i < rssiSampleCount; i++)
    {
        size_t index = (rssiWriteIndex + MAX_RSSI_SAMPLES - rssiSampleCount + i) % MAX_RSSI_SAMPLES;
        json += String(rssiHistory[index]);
        if (i + 1 < rssiSampleCount)
            json += ",";
    }
    json += "]";

    json += "}";
    return json;
}

// == html builders

String buildStatusBannerHtml()
{
    String status = currentHealthStatus();
    String cssClass = "banner-ok";

    if (status == "degraded")
        cssClass = "banner-warn";
    if (status == "down")
        cssClass = "banner-bad";

    String html;
    html += "<div class='banner ";
    html += cssClass;
    html += "'>";
    html += "<strong>Status: ";
    html += status;
    html += "</strong>";
    html += "<span>Reason: ";
    html += healthReasonText();
    html += "</span>";
    html += "<span>Reconnects last 60s: ";
    html += String(reconnectsInWindow(RECONNECT_RATE_WINDOW_MS));
    html += "</span>";
    html += "</div>";

    return html;
}

String buildRssiChartSvg()
{
    const int width = 360;
    const int height = 140;
    const int padLeft = 36;
    const int padRight = 12;
    const int padTop = 12;
    const int padBottom = 24;

    const int plotWidth = width - padLeft - padRight;
    const int plotHeight = height - padTop - padBottom;

    const int rssiMin = -100;
    const int rssiMax = -30;

    String svg;
    svg.reserve(3000);

    svg += "<svg viewBox='0 0 ";
    svg += String(width);
    svg += " ";
    svg += String(height);
    svg += "' width='100%' height='160' xmlns='http://www.w3.org/2000/svg'>";

    svg += "<rect x='0' y='0' width='100%' height='100%' rx='10' fill='#0f172a'/>";

    const int gridLevels[4] = {-90, -70, -50, -30};
    for (int i = 0; i < 4; i++)
    {
        int level = gridLevels[i];
        float ratio = float(level - rssiMin) / float(rssiMax - rssiMin);
        int y = padTop + plotHeight - int(ratio * plotHeight);

        svg += "<line x1='" + String(padLeft) + "' y1='" + String(y);
        svg += "' x2='" + String(width - padRight) + "' y2='" + String(y);
        svg += "' stroke='#334155' stroke-width='1' stroke-dasharray='4 4'/>";

        svg += "<text x='4' y='" + String(y + 4);
        svg += "' fill='#94a3b8' font-size='10'>";
        svg += String(level);
        svg += " dBm</text>";
    }

    svg += "<line x1='" + String(padLeft) + "' y1='" + String(padTop);
    svg += "' x2='" + String(padLeft) + "' y2='" + String(padTop + plotHeight);
    svg += "' stroke='#64748b' stroke-width='1'/>";

    svg += "<line x1='" + String(padLeft) + "' y1='" + String(padTop + plotHeight);
    svg += "' x2='" + String(width - padRight) + "' y2='" + String(padTop + plotHeight);
    svg += "' stroke='#64748b' stroke-width='1'/>";

    if (rssiSampleCount == 0)
    {
        svg += "<text x='50%' y='50%' text-anchor='middle' fill='#94a3b8' font-size='14'>No RSSI samples yet</text>";
        svg += "</svg>";
        return svg;
    }

    String points;
    points.reserve(1200);

    for (size_t i = 0; i < rssiSampleCount; i++)
    {
        size_t index = (rssiWriteIndex + MAX_RSSI_SAMPLES - rssiSampleCount + i) % MAX_RSSI_SAMPLES;
        int rssi = clampInt((int)rssiHistory[index], rssiMin, rssiMax);

        float xRatio = rssiSampleCount == 1 ? 0.5f : float(i) / float(rssiSampleCount - 1);
        float yRatio = float(rssi - rssiMin) / float(rssiMax - rssiMin);

        int x = padLeft + int(xRatio * plotWidth);
        int y = padTop + plotHeight - int(yRatio * plotHeight);

        points += String(x) + "," + String(y);
        if (i + 1 < rssiSampleCount)
            points += " ";
    }

    svg += "<polyline fill='none' stroke='#38bdf8' stroke-width='3' points='";
    svg += points;
    svg += "'/>";

    size_t lastIndex = (rssiWriteIndex + MAX_RSSI_SAMPLES - 1) % MAX_RSSI_SAMPLES;
    svg += "<text x='" + String(width - padRight - 4);
    svg += "' y='16' text-anchor='end' fill='#e2e8f0' font-size='12'>";
    svg += "Current: " + String(rssiHistory[lastIndex]) + " dBm</text>";

    svg += "</svg>";
    return svg;
}

String buildLandingHtml()
{
    String html;
    html.reserve(1600);

    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>SixEyes</title>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:24px;background:#0f172a;color:#e2e8f0;}";
    html += "a{color:#38bdf8;text-decoration:none;}a:hover{text-decoration:underline;}";
    html += ".card{background:#1e293b;padding:18px;border-radius:12px;max-width:760px;}";
    html += "li{margin:8px 0;}";
    html += "</style></head><body>";
    html += "<div class='card'>";
    html += "<h1>SixEyes</h1>";
    html += "<p>ESP32-hosted Wi-Fi telemetry dashboard and lightweight HTTP API.</p>";
    html += "<ul>";
    html += "<li><a href='/dashboard'>/dashboard</a> - dashboard</li>";
    html += "<li><a href='/api/health'>/api/health</a> - health model</li>";
    html += "<li><a href='/api/metrics'>/api/metrics</a> - telemetry schema</li>";
    html += "<li><a href='/api/events'>/api/events</a> - event stream</li>";
    html += "<li><a href='/api/info'>/api/info</a> - device metadata</li>";
    html += "</ul>";
    html += "</div></body></html>";

    return html;
}

String buildDashboardHtml()
{
    String html;
    html.reserve(13000);

    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "<title>SixEyes Dashboard</title>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:16px;background:#0f172a;color:#e2e8f0;}";
    html += "h1,h2{margin-top:0;}";
    html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px;}";
    html += ".card{background:#1e293b;border-radius:14px;padding:16px;box-shadow:0 2px 8px rgba(0,0,0,.25);}";
    html += ".ok{color:#22c55e;font-weight:bold;}.bad{color:#ef4444;font-weight:bold;}.warn{color:#f59e0b;font-weight:bold;}";
    html += ".muted{color:#94a3b8;}.mono{font-family:monospace;}";
    html += "table{width:100%;border-collapse:collapse;}th,td{text-align:left;padding:8px;border-bottom:1px solid #334155;vertical-align:top;}";
    html += ".banner{padding:14px 16px;border-radius:14px;margin-bottom:16px;display:flex;gap:18px;flex-wrap:wrap;}";
    html += ".banner-ok{background:#064e3b;color:#dcfce7;}.banner-warn{background:#78350f;color:#fffbeb;}.banner-bad{background:#7f1d1d;color:#fee2e2;}";
    html += "a{color:#38bdf8;}";
    html += "</style></head><body>";

    html += "<h1>SixEyes Wi-Fi Telemetry Dashboard</h1>";
    html += "<p class='muted'>Auto-refresh every 5 seconds</p>";
    html += buildStatusBannerHtml();

    html += "<div class='grid'>";

    html += "<div class='card'><h2>Connection</h2>";
    html += "<p>Status: <span class='" + String(wifiState.connected ? "ok" : "bad") + "'>" + wifiState.statusText + "</span></p>";
    html += "<p>SSID: <span class='mono'>" + wifiState.ssid + "</span></p>";
    html += "<p>Hostname: <span class='mono'>" + wifiState.hostname + "</span></p>";
    html += "<p>RSSI: <span class='mono'>" + String(wifiState.rssi) + " dBm</span></p>";
    html += "<p>Reconnect count: <span class='mono'>" + String(wifiState.reconnectCount) + "</span></p>";
    html += "<p>Reconnects last 60s: <span class='mono'>" + String(reconnectsInWindow(RECONNECT_RATE_WINDOW_MS)) + "</span></p>";
    html += "</div>";

    html += "<div class='card'><h2>IP Info</h2>";
    html += "<p>Local IP: <span class='mono'>" + redactableIP(wifiState.localIP) + "</span></p>";
    html += "<p>Gateway: <span class='mono'>" + redactableIP(wifiState.gatewayIP) + "</span></p>";
    html += "<p>Subnet: <span class='mono'>" + redactableIP(wifiState.subnetMask) + "</span></p>";
    html += "<p>DNS 1: <span class='mono'>" + redactableIP(wifiState.dns1) + "</span></p>";
    html += "<p>DNS 2: <span class='mono'>" + redactableIP(wifiState.dns2) + "</span></p>";
    html += "</div>";

    html += "<div class='card'><h2>System</h2>";
    html += "<p>Uptime: <span class='mono'>" + formatUptime(systemState.uptimeMs) + "</span></p>";
    html += "<p>Free heap: <span class='mono'>" + String(systemState.freeHeap) + " bytes</span></p>";
    html += "<p>SDK: <span class='mono'>" + systemState.sdkVersion + "</span></p>";
    html += "<p>Core: <span class='mono'>" + systemState.coreVersion + "</span></p>";
    html += "</div>";

    html += "<div class='card'><h2>Last Disconnect</h2>";
    html += "<p>Reason code: <span class='mono'>" + String(wifiState.lastDisconnectReason) + "</span></p>";
    html += "<p>Reason text: <span class='mono'>" + disconnectReasonToString(wifiState.lastDisconnectReason) + "</span></p>";
    html += "</div>";

    html += "</div>";

    html += "<div class='grid' style='margin-top:16px;'>";

    html += "<div class='card'><h2>RSSI History</h2>";
    html += "<p class='muted'>Recent Wi-Fi signal strength samples</p>";
    html += buildRssiChartSvg();
    html += "</div>";

    html += "<div class='card'><h2>Recent Events</h2>";
    if (eventCount == 0)
    {
        html += "<p class='muted'>No events recorded yet.</p>";
    }
    else
    {
        html += "<table><thead><tr><th>ts(ms)</th><th>level</th><th>type</th><th>message</th></tr></thead><tbody>";
        for (size_t i = 0; i < eventCount; i++)
        {
            size_t index = (eventWriteIndex + MAX_EVENTS - eventCount + i) % MAX_EVENTS;
            html += "<tr>";
            html += "<td class='mono'>" + String(eventLog[index].tsMs) + "</td>";
            html += "<td>" + eventLog[index].level + "</td>";
            html += "<td class='mono'>" + eventLog[index].type + "</td>";
            html += "<td>" + eventLog[index].message + "</td>";
            html += "</tr>";
        }
        html += "</tbody></table>";
    }
    html += "</div>";

    html += "</div>";

    html += "<p style='margin-top:16px;'>";
    html += "<a href='/api/metrics'>/api/metrics</a> | ";
    html += "<a href='/api/health'>/api/health</a> | ";
    html += "<a href='/api/events'>/api/events</a> | ";
    html += "<a href='/api/info'>/api/info</a>";
    html += "</p>";

    html += "</body></html>";
    return html;
}

// == web handlers

void handleRoot()
{
    server.send(200, "text/html", buildLandingHtml());
}

void handleDashboard()
{
    server.send(200, "text/html", buildDashboardHtml());
}

void sendJson(int statusCode, const String &body)
{
    server.sendHeader("Cache-Control", "no-store");
    server.send(statusCode, "application/json", body);
}

void handleHealth()
{
    sendJson(200, buildHealthJson());
}

void handleMetrics()
{
    sendJson(200, buildMetricsJson());
}

void handleEvents()
{
    sendJson(200, buildEventsJson());
}

void handleInfo()
{
    sendJson(200, buildInfoJson());
}

void handleSecret()
{
    static const char *heroes[] = {
        "Satoru Gojo - The Honored one",
        "Ryomen Sukuna - King of Curses ",
        "Yuji Itadori - The Vessel",
        "Megumi Fushiguro - Ten Shadow User",
        "Yuta Okkotsu - The Cursed Child",
        "Toji Fushiguro - The Outlier",
        "Maki Zenin - The Outlier Part 2",
    };

    size_t count = sizeof(heroes) / sizeof(heroes[0]);
    size_t index = (millis() / 1000UL) % count;

    String json = "{";
    json += "\"secret\":\"" + jsonEscape(String(heroes[index])) + "\"";
    json += "}";

    server.send(200, "application/json", json);
}

void handleNotFound()
{
    String json = "{";
    json += "\"error\":\"not_found\",";
    json += "\"path\":\"" + jsonEscape(server.uri()) + "\"";
    json += "}";

    sendJson(404, json);
}

void setupRoutes()
{
    server.on("/", handleRoot);
    server.on("/dashboard", handleDashboard);

    server.on("/api/health", handleHealth);
    server.on("/api/metrics", handleMetrics);
    server.on("/api/events", handleEvents);
    server.on("/api/info", handleInfo);
    // server.on("/api/secret", handleSecret);

    // Backwards-compatible aliases
    server.on("/health", handleHealth);
    server.on("/metrics", handleMetrics);
    // server.on("/secret", handleSecret);

    server.onNotFound(handleNotFound);
    server.begin();

    addEvent("INFO", "http_server_started", "HTTP server started on port " + String(HTTP_PORT));
}

// == setup / loop

void setup()
{
    Serial.begin(115200);
    delay(1000);

    wifiState.connected = false;
    wifiState.statusText = "BOOTING";
    wifiState.ssid = "";
    wifiState.hostname = DEVICE_HOSTNAME;
    wifiState.rssi = 0;
    wifiState.reconnectCount = 0;
    wifiState.lastDisconnectReason = -1;
    wifiState.lastSampleMs = 0;

    systemState.uptimeMs = 0;
    systemState.freeHeap = 0;
    systemState.sdkVersion = "";
    systemState.coreVersion = "";

    addEvent("INFO", "boot", "SixEyes telemetry agent booting");

    WiFi.onEvent(onWiFiEvent);

    connectToWiFi();
    sampleTelemetry();
    setupRoutes();

    addEvent("INFO", "setup_complete", "Setup complete");
}

void loop()
{
    unsigned long now = millis();

    server.handleClient();
    maybeReconnectWiFi();

    if (now - lastTelemetrySampleMs >= TELEMETRY_SAMPLE_INTERVAL_MS)
    {
        lastTelemetrySampleMs = now;
        sampleTelemetry();
    }

    delay(10);
}