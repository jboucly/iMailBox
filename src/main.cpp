/*
 * iMailBox - notification Discord à l'arrivée du courrier.
 *
 * Fonctionnement (matériel réel) :
 *   PIR -> transistor NPN -> MOSFET P -> alimente l'ESP8266. Au boot, l'ESP met HOLD_PIN à HIGH
 *   pour maintenir sa propre alimentation (auto-maintien), envoie la notification,
 *   puis relâche HOLD_PIN : l'alimentation est coupée, consommation ~0.
 *
 * Anti-spam : l'heure (NTP) du dernier envoi est stockée en flash (LittleFS),
 * donc elle survit aux coupures d'alimentation. Les statistiques aussi.
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include "secrets.h"

#ifdef ESP32
#include <WiFi.h>
#include <HTTPClient.h>
const int PIR_PIN = 14;
const int HOLD_PIN = 13;
const int BATTERY_PIN = 34;         // potentiomètre dans Wokwi
const float ADC_FULL_SCALE_V = 4.2; // 4095 -> 4,2 V (simulation)
const int ADC_MAX = 4095;
#else
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
const int PIR_PIN = 14;  // D5
const int HOLD_PIN = 12; // D6 -> base du transistor d'auto-maintien
const int BATTERY_PIN = A0;
// Pont interne du D1 Mini Pro (220k/100k) + R5 100k externe -> 1023 = ~4,2 V.
// Calibrer : mesurer VBAT au multimètre et ajuster cette valeur.
const float ADC_FULL_SCALE_V = 4.2;
const int ADC_MAX = 1023;
#endif

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long NTP_TIMEOUT_MS = 5000;
const time_t COOLDOWN_S = 10;//5 * 60; // pas plus d'une notification toutes les 5 min
const int LOW_BATTERY_PERCENT = 20;
const char *STATS_FILE = "/stats";

// Persisté en flash entre deux réveils
struct Stats
{
  time_t lastSent = 0;   // heure du dernier envoi réussi
  uint32_t wakeups = 0;  // nombre total de réveils (détections)
  uint32_t sent = 0;     // notifications envoyées
  uint32_t failures = 0; // échecs Wi-Fi / HTTP
  time_t firstBoot = 0;  // date de mise en service
  float lastVoltage = 0; // tension au réveil précédent
};

struct Battery
{
  float voltage;
  int percent;
};

Stats stats;
Battery battery;
unsigned long wifiConnectMs = 0;

// ---------- Batterie ----------

// Courbe de décharge typique d'une LiPo 1S au repos
int lipoPercent(float v)
{
  static const float volts[] = {3.30, 3.50, 3.60, 3.70, 3.75, 3.80, 3.85, 3.90, 4.00, 4.10, 4.20};
  static const int pct[] = {0, 5, 10, 20, 30, 40, 50, 60, 75, 90, 100};
  const int n = sizeof(volts) / sizeof(volts[0]);
  if (v <= volts[0])
    return 0;
  if (v >= volts[n - 1])
    return 100;
  for (int i = 1; i < n; i++)
  {
    if (v < volts[i])
      return pct[i - 1] + (v - volts[i - 1]) * (pct[i] - pct[i - 1]) / (volts[i] - volts[i - 1]);
  }
  return 100;
}

// À appeler avant d'activer le Wi-Fi : les pics de courant faussent la mesure
Battery readBattery()
{
  long sum = 0;
  for (int i = 0; i < 16; i++)
  {
    sum += analogRead(BATTERY_PIN);
    delay(2);
  }
  float v = (sum / 16.0) * ADC_FULL_SCALE_V / ADC_MAX;
  return {v, lipoPercent(v)};
}

// ---------- Statistiques (LittleFS) ----------

void loadStats()
{
  File f = LittleFS.open(STATS_FILE, "r");
  if (!f)
    return;
  if (f.size() == sizeof(Stats))
    f.read((uint8_t *)&stats, sizeof(Stats));
  f.close();
}

void saveStats()
{
  File f = LittleFS.open(STATS_FILE, "w");
  if (!f)
    return;
  f.write((const uint8_t *)&stats, sizeof(Stats));
  f.close();
}

// ---------- Réseau ----------

bool connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - start > WIFI_TIMEOUT_MS)
    {
      Serial.println("\nÉchec de connexion Wi-Fi");
      return false;
    }
    delay(250);
    Serial.print(".");
  }
  wifiConnectMs = millis() - start;
  Serial.printf("\nWi-Fi connecté (%s, RSSI %d dBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// Retourne l'heure Unix, ou 0 si NTP indisponible
time_t syncTime()
{
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  unsigned long start = millis();
  time_t now;
  while ((now = time(nullptr)) < 1700000000)
  {
    if (millis() - start > NTP_TIMEOUT_MS)
      return 0;
    delay(100);
  }
  return now;
}

String isoTime(time_t t)
{
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
  return String(buf);
}

String batteryBar(int percent)
{
  String bar;
  int filled = (percent + 5) / 10;
  for (int i = 0; i < 10; i++)
    bar += i < filled ? "█" : "░";
  return bar;
}

bool sendDiscordNotification(time_t now)
{
  // Couleur de l'embed selon la batterie : vert / orange / rouge
  int color = battery.percent > 50 ? 0x2ECC71 : battery.percent > LOW_BATTERY_PERCENT ? 0xF39C12
                                                                                      : 0xE74C3C;

  String trend = "";
  if (stats.lastVoltage > 0)
  {
    float delta = battery.voltage - stats.lastVoltage;
    trend = String(delta >= 0 ? " (+" : " (") + String(delta * 1000, 0) + " mV)";
  }

  unsigned long days = (now && stats.firstBoot) ? (now - stats.firstBoot) / 86400 : 0;

  String desc = "Du courrier vient d'arriver !";
  if (battery.percent <= LOW_BATTERY_PERCENT)
    desc += "\\n⚠️ **Batterie faible** : vérifie le panneau solaire.";

  char json[1200];
  snprintf(json, sizeof(json),
           "{\"embeds\":[{"
           "\"title\":\"📬 Boîte aux lettres\","
           "\"description\":\"%s\","
           "\"color\":%d,"
           "\"fields\":["
           "{\"name\":\"🔋 Batterie\",\"value\":\"%s %d %%\\n%.2f V%s\",\"inline\":false},"
           "{\"name\":\"📶 Wi-Fi\",\"value\":\"%d dBm\\nconnexion %lu ms\",\"inline\":true},"
           "{\"name\":\"📊 Activité\",\"value\":\"%lu réveils\\n%lu notifs, %lu échecs\",\"inline\":true},"
           "{\"name\":\"🗓️ En service\",\"value\":\"%lu jours\",\"inline\":true}"
           "]%s}]}",
           desc.c_str(), color,
           batteryBar(battery.percent).c_str(), battery.percent, battery.voltage, trend.c_str(),
           WiFi.RSSI(), wifiConnectMs,
           (unsigned long)stats.wakeups, (unsigned long)stats.sent, (unsigned long)stats.failures,
           days,
           now ? String(",\"timestamp\":\"" + isoTime(now) + "\"").c_str() : "");

  WiFiClientSecure client;
  client.setInsecure(); // pas de vérification du certificat (suffisant ici)

  HTTPClient http;
  http.begin(client, DISCORD_WEBHOOK_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(String(json));
  http.end();

  // Discord répond 204 No Content en cas de succès
  if (code >= 200 && code < 300)
  {
    Serial.printf("Notification envoyée (HTTP %d)\n", code);
    return true;
  }
  Serial.printf("Erreur d'envoi : %d %s\n", code, http.errorToString(code).c_str());
  return false;
}

void handleMailEvent()
{
  battery = readBattery(); // avant le Wi-Fi
  Serial.printf("Batterie : %.2f V (%d %%)\n", battery.voltage, battery.percent);

  loadStats();
  stats.wakeups++;

  if (!connectWiFi())
  {
    stats.failures++;
    saveStats();
    return;
  }

  time_t now = syncTime();
  if (now && !stats.firstBoot)
    stats.firstBoot = now;

  if (now && stats.lastSent && now - stats.lastSent < COOLDOWN_S)
  {
    Serial.printf("Anti-spam : dernière notification il y a %lds, ignorée\n", (long)(now - stats.lastSent));
    saveStats();
    return;
  }

  if (sendDiscordNotification(now))
  {
    stats.sent++;
    if (now)
      stats.lastSent = now;
  }
  else
  {
    stats.failures++;
  }
  stats.lastVoltage = battery.voltage;
  saveStats();
}

void powerOff()
{
  Serial.println("Coupure de l'alimentation");
  Serial.flush();
  WiFi.disconnect(true);
  digitalWrite(HOLD_PIN, LOW);
  // Si l'alimentation n'est pas coupée (PIR encore HIGH, ou alim USB), on attend
  // la fin de la détection avant de redonner la main.
  delay(500);
}

void setup()
{
  pinMode(HOLD_PIN, OUTPUT);
  digitalWrite(HOLD_PIN, HIGH); // auto-maintien de l'alimentation, le plus tôt possible
  pinMode(PIR_PIN, INPUT);

  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== iMailBox ===");

  if (!LittleFS.begin())
  {
#ifdef ESP32
    LittleFS.begin(true); // formatage au premier démarrage
#else
    LittleFS.format();
    LittleFS.begin();
#endif
  }

#ifdef SIMULATION
  Serial.println("Mode simulation : en attente du PIR...");
#else
  // Le réveil a été causé par le PIR : on traite immédiatement
  handleMailEvent();
  powerOff();
#endif
}

void loop()
{
  // Matériel réel : n'arrive ici que si l'alim n'a pas été coupée (ex. branché en USB).
  // Simulation : on détecte les fronts montants du PIR.
  static int lastState = LOW;
  int state = digitalRead(PIR_PIN);
  if (state == HIGH && lastState == LOW)
  {
    Serial.println("Mouvement détecté !");
    digitalWrite(HOLD_PIN, HIGH);
    handleMailEvent();
    powerOff();
  }
  lastState = state;
  delay(50);
}
