/*
============================================================
 SISTEMA DE RIEGO INTELIGENTE - ESP32 S3
 VERSION FINAL V3
============================================================

*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP32Servo.h>
#include "DHT.h"
#include <Preferences.h>
#include <EloquentTinyML.h>
#include "model.h"
#include <cmath>  

// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID     = "ONE-SALOME";
const char* WIFI_PASSWORD = "SanchoPanza30";

// ============================================================
// OPEN METEO
// ============================================================

const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=5.5353&longitude=-73.3678"
  "&current_weather=true"
  "&hourly=relativehumidity_2m,precipitation_probability"
  "&forecast_days=1";

// ============================================================
// PINES
// ============================================================

#define BUTTON_PIN   21
#define LED_R        10
#define LED_G        11
#define LED_B        12
#define DHTPIN        7
#define DHTTYPE   DHT11
#define HUM1_PIN     1
#define HUM2_PIN     2
#define WATER_PIN    3
#define RELAY_PIN    18
#define SERVO1_PIN   13
#define SERVO2_PIN   14

// ============================================================
// INTERVALOS
// ============================================================

#define INTERVALO_MUESTRA        60000UL
#define INTERVALO_LECTURA        1000UL
#define INTERVALO_OLED           5000UL
#define INTERVALO_METRICAS       10000UL
#define DEBOUNCE_MS              200UL
#define NUM_PANTALLAS            6

// ============================================================
// SCALER
// ============================================================

const float SCALER_MEAN[7]  = { 73.87761194f, 11.15671642f, 80.54701493f,
                                 10.9641791f,  92.7761194f,  0.87835821f,
                                 11.28358209f };
const float SCALER_SCALE[7] = {  4.89116843f,  2.37466251f,  6.68701895f,
                                  2.35576153f,  1.98742987f,  1.9047608f,
                                  6.79729088f };

// ============================================================
// ════════════════════════════════════════════════════════════
//  ESTRUCTURA DE DATOS #1 — BUFFER CIRCULAR (FIFO)
//  Complejidad: push O(1), pop O(1), promedio O(N)
//  Uso: almacena las últimas NUM_MUESTRAS lecturas del sensor
//       antes de pasarlas al modelo de IA.
// ════════════════════════════════════════════════════════════
// ============================================================

#define NUM_MUESTRAS  30

struct Muestra {
  float humSuelo;
  float temp;
  float humAmb;
  float tempWeb;
  float humWeb;
  float rain;
  float hora;
};

class BufferCircular {
public:
  // ── Constructor ──────────────────────────────────────────
  BufferCircular() : cabeza(0), cola(0), cantidad(0) {}

  // ── push: inserta una nueva muestra — O(1) ───────────────
  void push(const Muestra& m) {
    buffer[cola] = m;
    cola = (cola + 1) % NUM_MUESTRAS;
    if (cantidad < NUM_MUESTRAS) cantidad++;
    else cabeza = (cabeza + 1) % NUM_MUESTRAS; // sobrescribe el más viejo
  }

  // ── lleno: true cuando el buffer tiene NUM_MUESTRAS ──────
  bool lleno() const { return cantidad == NUM_MUESTRAS; }

  // ── size ─────────────────────────────────────────────────
  int size() const { return cantidad; }

  // ── promedio de cada campo — O(N) ────────────────────────
  Muestra promedio() const {
    Muestra prom = {0, 0, 0, 0, 0, 0, 0};
    if (cantidad == 0) return prom;
    for (int i = 0; i < cantidad; i++) {
      int idx = (cabeza + i) % NUM_MUESTRAS;
      prom.humSuelo += buffer[idx].humSuelo;
      prom.temp     += buffer[idx].temp;
      prom.humAmb   += buffer[idx].humAmb;
      prom.tempWeb  += buffer[idx].tempWeb;
      prom.humWeb   += buffer[idx].humWeb;
      prom.rain     += buffer[idx].rain;
      prom.hora     += buffer[idx].hora;
    }
    float n = (float)cantidad;
    prom.humSuelo /= n;  prom.temp    /= n;
    prom.humAmb   /= n;  prom.tempWeb /= n;
    prom.humWeb   /= n;  prom.rain    /= n;
    prom.hora     /= n;
    return prom;
  }

  // ── reset ────────────────────────────────────────────────
  void reset() { cabeza = cola = cantidad = 0; }

private:
  Muestra buffer[NUM_MUESTRAS];
  int cabeza, cola, cantidad;
};

// ============================================================
// OBJETOS
// ============================================================

DHT dht(DHTPIN, DHTTYPE);
Servo servo1, servo2;
Preferences prefs;
Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire);

BufferCircular bufferMuestras;   // ← estructura de datos #1

// ============================================================
// IA
// ============================================================

#define NUMBER_OF_INPUTS    7
#define NUMBER_OF_OUTPUTS   1
#define TENSOR_ARENA_SIZE   12*1024

Eloquent::TinyML::TfLite<
  NUMBER_OF_INPUTS,
  NUMBER_OF_OUTPUTS,
  TENSOR_ARENA_SIZE
> ml;

// ── Latencia de inferencia ──────────────────────────────────
// Promedio + stddev sobre las últimas 100 inferencias (R7)
#define MAX_LAT_MUESTRAS  100
float   latenciasMs[MAX_LAT_MUESTRAS];
int     latenciaIdx     = 0;
int     latenciaCount   = 0;
float   latenciaPromMs  = 0;
float   latenciaStdMs   = 0;
uint32_t latenciaMaxMs  = 0;

// ============================================================
// VARIABLES WEB
// ============================================================

float  wTemp = 0, wHum = 0, wWind = 0, wRain = 0;
int    wCode = 0;
String climaTexto = "N/A";

// ============================================================
// VARIABLES SENSORES
// ============================================================

float tempDHT = 0, humDHT = 0;
int   porcH1 = 0, porcH2 = 0, waterPct = 0;

// ============================================================
// IA
// ============================================================

String decisionIA  = "Esperando";
float  confianzaIA = 0;

// ============================================================
// ESTADOS
// ============================================================

bool wifiOK = false, sensoresOK = false, iaOK = false;
bool regandoManual = false, regandoAuto = false;

// ============================================================
// CONTADORES
// ============================================================

int vecesRegado = 0;

// ============================================================
// OLED
// ============================================================

int pantallaActual = 0;

// ============================================================
// TIMERS
// ============================================================

unsigned long ultimaLectura  = 0;
unsigned long ultimaMuestra  = 0;
unsigned long ultimaOLED     = 0;
unsigned long ultimaMetricas = 0;
unsigned long ultimoBoton    = 0;

// ============================================================
// MÉTRICAS DEL SISTEMA
// ============================================================

uint32_t ramLibre    = 0, ramTotal  = 0;
uint32_t ramUsada    = 0, ramMinLibre = 0;
uint32_t tiempoHTTP  = 0, tiempoHTTPMax = 0;
uint32_t tiempoLoop  = 0, tiempoLoopMax = 0;
int      rssiWifi    = 0;
uint32_t uptimeSeg   = 0;

// ============================================================
// PROTOTIPOS
// ============================================================

void  leerSensores();
bool  obtenerClima();
void  tomarMuestra();
void  ejecutarIA();
void  calcularLatencia(float ms);
void  activarRiego();
void  moverServos();
void  detenerServos();
void  actualizarOLED();
void  setLED(bool r, bool g, bool b);
float normalizar(float valor, int idx);
String descripcionClima(int code);
void  actualizarMetricas();
void  imprimirMetricasSerial();
String formatUptime(uint32_t seg);

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("============================================================");
  Serial.println(" SISTEMA DE RIEGO INTELIGENTE - ESP32 S3  v3");
  Serial.println("============================================================");

  prefs.begin("riego", false);
  vecesRegado = prefs.getInt("riegos", 0);
  decisionIA  = prefs.getString("decision", "Esperando");
  confianzaIA = prefs.getFloat("confIA", 0);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  Wire.begin(8, 9);
  display.begin(0x3C, true);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Iniciando v3...");
  display.display();

  dht.begin();

  servo1.attach(SERVO1_PIN); servo2.attach(SERVO2_PIN);
  servo1.write(0);           servo2.write(0);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Conectando");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500); Serial.print("."); intentos++;
  }
  wifiOK = (WiFi.status() == WL_CONNECTED);
  if (wifiOK) {
    Serial.printf("\n[WiFi] OK  IP:%s  RSSI:%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(-18000, 0, "pool.ntp.org");
  } else {
    Serial.println("\n[WiFi] ERROR");
  }

  ml.begin(modelo_riego);
  iaOK = true;
  Serial.println("[IA] Modelo cargado OK");

  ramTotal    = ESP.getHeapSize();
  ramMinLibre = ESP.getFreeHeap();
  Serial.printf("[RAM] Total:%u  Libre inicial:%u\n", ramTotal, ramMinLibre);

  leerSensores();
  obtenerClima();
  sensoresOK = true;
  setLED(false, false, true);
  actualizarOLED();
  Serial.println("[SYS] Sistema listo  (buffer circular activo)");
  Serial.println("============================================================");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  unsigned long inicioLoop = millis();
  unsigned long ahora      = inicioLoop;

  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = ahora;
    leerSensores();
  }

  if (digitalRead(BUTTON_PIN) == LOW &&
      !regandoManual && !regandoAuto &&
      (ahora - ultimoBoton) > DEBOUNCE_MS) {
    ultimoBoton   = ahora;
    regandoManual = true;
    decisionIA    = "Riego Manual";
    activarRiego();
    regandoManual = false;
  }

  if (ahora - ultimaMuestra >= INTERVALO_MUESTRA) {
    ultimaMuestra = ahora;
    obtenerClima();
    tomarMuestra();
    Serial.printf("[BUF] Muestras en buffer: %d/%d\n",
                  bufferMuestras.size(), NUM_MUESTRAS);

    if (bufferMuestras.lleno()) {
      ejecutarIA();
      bufferMuestras.reset();   // vacía el buffer para la siguiente ventana
    }
  }

  if (ahora - ultimaOLED >= INTERVALO_OLED) {
    ultimaOLED    = ahora;
    pantallaActual = (pantallaActual + 1) % NUM_PANTALLAS;
    actualizarOLED();
  }

  if (ahora - ultimaMetricas >= INTERVALO_METRICAS) {
    ultimaMetricas = ahora;
    actualizarMetricas();
    imprimirMetricasSerial();
  }

  // Watchdog WiFi
  if (!wifiOK && WiFi.status() == WL_CONNECTED) { wifiOK = true; }
  if ( wifiOK && WiFi.status() != WL_CONNECTED) { wifiOK = false; WiFi.reconnect(); }

  tiempoLoop = millis() - inicioLoop;
  if (tiempoLoop > tiempoLoopMax) tiempoLoopMax = tiempoLoop;

  delay(50);
}

// ============================================================
// LEER SENSORES
// ============================================================

void leerSensores() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) tempDHT = t;
  if (!isnan(h)) humDHT  = h;

  int raw1 = analogRead(HUM1_PIN); delay(2);
  int raw2 = analogRead(HUM2_PIN); delay(2);
  int rawW = analogRead(WATER_PIN); delay(2);

  porcH1   = constrain(map(raw1, 4095, 1300,  0, 100), 0, 100);
  porcH2   = constrain(map(raw2, 4095, 1300,  0, 100), 0, 100);
  waterPct = constrain(map(rawW,  250, 1750,  0, 100), 0, 100);
}

// ============================================================
// TOMAR MUESTRA → PUSH AL BUFFER CIRCULAR
// ============================================================

void tomarMuestra() {
  struct tm ti;
  int hora = 0;
  if (getLocalTime(&ti)) hora = ti.tm_hour;

  Muestra m;
  m.humSuelo = (porcH1 + porcH2) / 2.0f;
  m.temp     = tempDHT;
  m.humAmb   = humDHT;
  m.tempWeb  = wTemp;
  m.humWeb   = wHum;
  m.rain     = (wRain > 50 ? 1.0f : 0.0f);
  m.hora     = (float)hora;

  bufferMuestras.push(m);  // O(1)
}

// ============================================================
// NORMALIZAR
// ============================================================

float normalizar(float valor, int idx) {
  return (valor - SCALER_MEAN[idx]) / SCALER_SCALE[idx];
}

// ============================================================
// CALCULAR ESTADÍSTICAS DE LATENCIA
// ============================================================

void calcularLatencia(float ms) {
  latenciasMs[latenciaIdx] = ms;
  latenciaIdx = (latenciaIdx + 1) % MAX_LAT_MUESTRAS;
  if (latenciaCount < MAX_LAT_MUESTRAS) latenciaCount++;
  if ((uint32_t)ms > latenciaMaxMs) latenciaMaxMs = (uint32_t)ms;

  // Promedio
  float suma = 0;
  for (int i = 0; i < latenciaCount; i++) suma += latenciasMs[i];
  latenciaPromMs = suma / latenciaCount;

  // Desviación estándar
  float varianza = 0;
  for (int i = 0; i < latenciaCount; i++) {
    float d = latenciasMs[i] - latenciaPromMs;
    varianza += d * d;
  }
  latenciaStdMs = sqrt(varianza / latenciaCount);
}

// ============================================================
// EJECUTAR IA — usa el promedio del buffer circular
// ============================================================

void ejecutarIA() {
  Muestra prom = bufferMuestras.promedio();   // O(N)

  float input[7];
  input[0] = normalizar(prom.humSuelo, 0);
  input[1] = normalizar(prom.temp,     1);
  input[2] = normalizar(prom.humAmb,   2);
  input[3] = normalizar(prom.tempWeb,  3);
  input[4] = normalizar(prom.humWeb,   4);
  input[5] = normalizar(prom.rain,     5);
  input[6] = normalizar(prom.hora,     6);

  // ── Medir latencia de inferencia (R7) ───────────────────
  unsigned long t0 = millis();
  float output = ml.predict(input);
  float latMs  = (float)(millis() - t0);

  calcularLatencia(latMs);
  // ────────────────────────────────────────────────────────

  confianzaIA = output;
  prefs.putFloat("confIA", confianzaIA);

  Serial.printf("[IA] Salida:%.4f  Latencia:%.1f ms\n", output, latMs);

  if (output >= 0.5f) {
    decisionIA  = "REGAR";
    regandoAuto = true;
    activarRiego();
    regandoAuto = false;
  } else if (output >= 0.35f) {
    decisionIA = "POSIBLE";
  } else {
    decisionIA = "NO REGAR";
  }

  prefs.putString("decision", decisionIA);
  prefs.putFloat("humSuelo", prom.humSuelo);
  prefs.putFloat("tempProm", prom.temp);
  prefs.putFloat("humProm",  prom.humAmb);
  prefs.putFloat("tempWeb",  prom.tempWeb);
}

// ============================================================
// ACTIVAR RIEGO
// ============================================================

void activarRiego() {
  if (waterPct < 10) {
    decisionIA = "Sin agua";
    Serial.println("[RIEGO] Abortado: nivel bajo");
    return;
  }
  Serial.println("[RIEGO] Ciclo 30 s...");
  digitalWrite(RELAY_PIN, HIGH);
  setLED(false, true, false);
  delay(1000);

  unsigned long inicio = millis();
  while (millis() - inicio < 30000) {
    moverServos();
    actualizarOLED();
    delay(100);
  }

  digitalWrite(RELAY_PIN, LOW);
  detenerServos();
  vecesRegado++;
  prefs.putInt("riegos", vecesRegado);
  setLED(false, false, true);
  Serial.printf("[RIEGO] Completado. Total:%d\n", vecesRegado);
}

// ============================================================
// SERVOS
// ============================================================

void moverServos() {
  static int angulo = 0, direccion = 1;
  static unsigned long ultimo = 0;
  if (millis() - ultimo >= 500) {
    ultimo = millis();
    angulo += direccion * 20;
    if (angulo >= 180) { angulo = 180; direccion = -1; }
    if (angulo <=   0) { angulo =   0; direccion =  1; }
    servo1.write(angulo);
    servo2.write(angulo);
  }
}

void detenerServos() { servo1.write(0); servo2.write(0); }

// ============================================================
// LED
// ============================================================

void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_R, r);
  digitalWrite(LED_G, g);
  digitalWrite(LED_B, b);
}

// ============================================================
// MÉTRICAS
// ============================================================

void actualizarMetricas() {
  ramLibre    = ESP.getFreeHeap();
  ramTotal    = ESP.getHeapSize();
  ramUsada    = ramTotal - ramLibre;
  if (ramLibre < ramMinLibre) ramMinLibre = ramLibre;
  rssiWifi  = wifiOK ? WiFi.RSSI() : 0;
  uptimeSeg = millis() / 1000;
}

void imprimirMetricasSerial() {
  Serial.println();
  Serial.println("============================================================");
  Serial.printf(" MÉTRICAS  |  Uptime: %s\n", formatUptime(uptimeSeg).c_str());
  Serial.println("------------------------------------------------------------");

  // RAM
  Serial.println("[RAM]");
  Serial.printf("  Total        : %u bytes (%.1f KB)\n",  ramTotal,    ramTotal/1024.0f);
  Serial.printf("  En uso       : %u bytes (%.1f KB) %.1f%%\n",
                ramUsada, ramUsada/1024.0f, 100.0f*ramUsada/ramTotal);
  Serial.printf("  Libre        : %u bytes (%.1f KB)\n",  ramLibre,    ramLibre/1024.0f);
  Serial.printf("  Mínimo hist. : %u bytes (%.1f KB)\n",  ramMinLibre, ramMinLibre/1024.0f);

  // CPU
  Serial.println("[CPU]");
  Serial.printf("  Frecuencia   : %u MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("  Flash total  : %.1f MB\n", ESP.getFlashChipSize()/1048576.0f);
  Serial.printf("  Sketch       : %u KB / %u KB libres\n",
                ESP.getSketchSize()/1024, ESP.getFreeSketchSpace()/1024);

  // Latencia inferencia (R7)
  Serial.println("[INFERENCIA IA]");
  Serial.printf("  Ultima       : %.1f ms\n",
                latenciaCount > 0 ? latenciasMs[(latenciaIdx-1+MAX_LAT_MUESTRAS)%MAX_LAT_MUESTRAS] : 0);
  Serial.printf("  Promedio     : %.2f ms  (N=%d)\n", latenciaPromMs, latenciaCount);
  Serial.printf("  Std dev      : %.2f ms\n", latenciaStdMs);
  Serial.printf("  Máximo       : %u ms\n",  latenciaMaxMs);

  // Buffer circular
  Serial.println("[BUFFER CIRCULAR]");
  Serial.printf("  Capacidad    : %d muestras\n", NUM_MUESTRAS);
  Serial.printf("  Ocupación    : %d muestras\n", bufferMuestras.size());
  Serial.printf("  push/pop     : O(1)  |  promedio: O(N)\n");

  // Loop
  Serial.println("[LOOP]");
  Serial.printf("  Último ciclo : %u ms\n", tiempoLoop);
  Serial.printf("  Máx. ciclo   : %u ms\n", tiempoLoopMax);

  // HTTP / WiFi
  Serial.println("[HTTP / WiFi]");
  Serial.printf("  Última resp. : %u ms\n", tiempoHTTP);
  Serial.printf("  Máx. resp.   : %u ms\n", tiempoHTTPMax);
  if (wifiOK) {
    Serial.printf("  RSSI         : %d dBm\n", rssiWifi);
    Serial.printf("  IP           : %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("  WiFi         : DESCONECTADO");
  }

  // Sensores
  Serial.println("[SENSORES]");
  Serial.printf("  Temp         : %.1f C  |  HumAmb: %.1f%%\n", tempDHT, humDHT);
  Serial.printf("  Suelo P1     : %d%%    |  P2: %d%%\n", porcH1, porcH2);
  Serial.printf("  Nivel agua   : %d%%\n", waterPct);

  // IA
  Serial.println("[IA]");
  Serial.printf("  Decision     : %s\n", decisionIA.c_str());
  Serial.printf("  Confianza    : %.4f\n", confianzaIA);
  Serial.printf("  Total riegos : %d\n", vecesRegado);

  Serial.println("============================================================");
  Serial.println();
}

// ============================================================
// FORMAT UPTIME
// ============================================================

String formatUptime(uint32_t seg) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u",
           seg/86400, (seg%86400)/3600, (seg%3600)/60, seg%60);
  return String(buf);
}

// ============================================================
// OLED — 6 pantallas
// ============================================================

void actualizarOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  // ── 0: Estado general ────────────────────────────────────
  if (pantallaActual == 0) {
    display.setCursor(0,0); display.println("RIEGO INTELIGENTE");
    display.drawLine(0,10,127,10,SH110X_WHITE);
    display.setCursor(0,14); display.print("WiFi: "); display.println(wifiOK?"OK":"ERR");
    display.setCursor(0,24); display.print("IA:   "); display.println(iaOK?"ACTIVA":"ERR");
    display.setCursor(0,34); display.print("Sens: "); display.println(sensoresOK?"OK":"ERR");
    display.setCursor(0,44); display.print("Riegos:"); display.println(vecesRegado);
    display.setCursor(0,54); display.print("Up:"); display.println(formatUptime(millis()/1000));
  }

  // ── 1: Sensores ──────────────────────────────────────────
  else if (pantallaActual == 1) {
    display.setCursor(0,0); display.println("SENSORES");
    display.drawLine(0,10,127,10,SH110X_WHITE);
    display.setCursor(0,14); display.print("Temp:"); display.print(tempDHT,1); display.println("C");
    display.setCursor(0,24); display.print("HumAmb:"); display.print(humDHT,0); display.println("%");
    display.setCursor(0,34); display.print("P1:"); display.print(porcH1);
                             display.print("% P2:"); display.print(porcH2); display.println("%");
    display.setCursor(0,44); display.print("Agua:"); display.print(waterPct); display.println("%");
  }

  // ── 2: Clima ─────────────────────────────────────────────
  else if (pantallaActual == 2) {
    display.setCursor(0,0); display.println("CLIMA WEB");
    display.drawLine(0,10,127,10,SH110X_WHITE);
    display.setCursor(0,14); display.println(climaTexto);
    display.setCursor(0,24); display.print("Temp:"); display.print(wTemp,1); display.println("C");
    display.setCursor(0,34); display.print("Viento:"); display.print(wWind,0); display.println("km/h");
    display.setCursor(0,44); display.print("Lluvia:");
    if (wRain<20) display.println("Baja");
    else if (wRain<50) display.println("Media");
    else display.println("Alta");
  }

  // ── 3: IA / Decisión ─────────────────────────────────────
  else if (pantallaActual == 3) {
    display.setCursor(0,0); display.println("IA / DECISION");
    display.drawLine(0,10,127,10,SH110X_WHITE);
    display.setCursor(0,14);
    display.print("Buffer:"); display.print(bufferMuestras.size());
    display.print("/"); display.println(NUM_MUESTRAS);
    display.setCursor(0,24); display.print("IA: "); display.println(decisionIA);
    display.setCursor(0,34); display.print("Conf:"); display.println(confianzaIA,3);
    display.setCursor(0,44); display.print("Riegos:"); display.println(vecesRegado);
  }

  // ── 4: Latencia inferencia (R7) ──────────────────────────
  else if (pantallaActual == 4) {
    display.setCursor(0,0); display.println("LATENCIA IA");
    display.drawLine(0,10,127,10,SH110X_WHITE);
    display.setCursor(0,14);
    display.print("Prom:"); display.print(latenciaPromMs,1); display.println("ms");
    display.setCursor(0,24);
    display.print("Std: "); display.print(latenciaStdMs,1); display.println("ms");
    display.setCursor(0,34);
    display.print("Max: "); display.print(latenciaMaxMs); display.println("ms");
    display.setCursor(0,44);
    display.print("N="); display.println(latenciaCount);
  }

  // ── 5: Sistema ESP32 ─────────────────────────────────────
  else {
    actualizarMetricas();
    display.setCursor(0,0); display.println("SISTEMA ESP32");
    display.drawLine(0,10,127,10,SH110X_WHITE);
    display.setCursor(0,14);
    display.print("RAM:"); display.print(ramLibre/1024); display.println("KB libre");
    display.setCursor(0,24);
    display.print("Uso:"); display.print((int)(100.0f*ramUsada/ramTotal)); display.println("%");
    display.setCursor(0,34);
    display.print("HTTP:"); display.print(tiempoHTTP); display.println("ms");
    display.setCursor(0,44);
    display.print("Loop:"); display.print(tiempoLoop); display.println("ms");
    display.setCursor(0,54);
    if (wifiOK) { display.print("RSSI:"); display.print(rssiWifi); display.println("dBm"); }
    else display.println("WiFi:OFF");
  }

  display.display();
}

// ============================================================
// CLIMA WEB
// ============================================================

bool obtenerClima() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(WEATHER_URL);
  http.setTimeout(8000);

  unsigned long t0 = millis();
  int code = http.GET();
  tiempoHTTP = millis() - t0;
  if (tiempoHTTP > tiempoHTTPMax) tiempoHTTPMax = tiempoHTTP;

  if (code != 200) {
    Serial.printf("[HTTP] Error %d (%u ms)\n", code, tiempoHTTP);
    http.end(); return false;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("[JSON] Error parse"); return false;
  }

  wTemp      = doc["current_weather"]["temperature"].as<float>();
  wWind      = doc["current_weather"]["windspeed"].as<float>();
  wCode      = doc["current_weather"]["weathercode"].as<int>();
  climaTexto = descripcionClima(wCode);

  JsonArray hum_arr  = doc["hourly"]["relativehumidity_2m"];
  JsonArray rain_arr = doc["hourly"]["precipitation_probability"];

  struct tm ti; int hora = 0;
  if (getLocalTime(&ti)) hora = ti.tm_hour;
  if (hora < (int)hum_arr.size())  wHum  = hum_arr[hora].as<float>();
  if (hora < (int)rain_arr.size()) wRain = rain_arr[hora].as<float>();

  return true;
}

// ============================================================
// DESCRIPCIÓN CLIMA
// ============================================================

String descripcionClima(int c) {
  if (c == 0)              return "Despejado";
  if (c >= 1  && c <= 3)  return "Nublado";
  if (c >= 45 && c <= 48) return "Neblina";
  if (c >= 51 && c <= 67) return "Lluvia";
  if (c >= 80 && c <= 82) return "Chubascos";
  if (c >= 95 && c <= 99) return "Tormenta";
  return "Desconocido";
}