#include <WiFiS3.h>
#include <WiFiUDP.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <EEPROM.h>
#include "DHT.h"
#include <stdint.h>
#include <math.h>
#include <HX711.h>
#include <NTPClient.h>
#include <time.h>

// ==================== WiFi配置 ====================
const char* WIFI_SSID     = "CMCC-gpGm";
const char* WIFI_PASSWORD = "cWxWEmPZ";
// ==================================================

WiFiServer webServer(80);
#define DHT_PIN 2
DHT dht(DHT_PIN, DHT11);

// ==================== BMP280 I2C ====================
Adafruit_BMP280 bmp;
bool bmpAvailable = false;
#define BMP280_ADDR 0x76
// =====================================================

// 传感器引脚定义
#define WIND_PIN         A2
#define WIND_DIR_PIN     A3

// HX711 称重雨量
#define HX711_DT  5
#define HX711_SCK 6
HX711 scale;
float scaleFactor = 679.0f;
float tareOffset = 0;
float lastWaterGram = 0;
float deltaRainMM = 0;
const float COLLECTOR_MM = 94.0f;
const float RAIN_DELTA_THRESHOLD = 0.10f;

// ==================== 极大风速 ====================
float windMaxInInterval = 0.0f;
unsigned long lastWindSample = 0;
const unsigned long WIND_SAMPLE_MS = 1000;
// ==================================================

// ==================== 安全读取 ====================
float lastValidPressureKpa = 101.325f;
float lastValidTempC = 25.0f;
float lastValidHum = 50.0f;
uint32_t pressureErrorCount = 0;
uint32_t temperatureErrorCount = 0;
const float PRESS_MIN_KPA = 90.0f;
const float PRESS_MAX_KPA = 110.0f;
const float TEMP_MIN_C = -40.0f;
const float TEMP_MAX_C = 85.0f;

// BMP 与 DHT 温差阈值（℃）
const float TEMP_DIFF_THRESHOLD = 2.5f;

// DHT 统一缓存
float cachedDhtTemp = NAN;
float cachedDhtHum = NAN;
unsigned long lastDhtRead = 0;
const unsigned long DHT_READ_INTERVAL = 3000;  // 至少3秒读一次DHT

// 统一读取 DHT 温湿度，避免单总线冲突
void readDhtBoth(float* outT, float* outH) {
  for (int i = 0; i < 3; i++) {
    float rt = dht.readTemperature();
    float rh = dht.readHumidity();
    if (!isnan(rt) && !isnan(rh) && rt > TEMP_MIN_C && rt < 80.0f && rh >= 0.0f && rh <= 100.0f) {
      cachedDhtTemp = rt;
      cachedDhtHum = rh;
      *outT = rt;
      *outH = rh;
      return;
    }
    delay(1200);
  }
  Serial.println("[DHT11] 读取失败，使用缓存值");
  *outT = cachedDhtTemp;
  *outH = cachedDhtHum;
}

// 带缓存的 DHT 读取触发
void updateDhtCache() {
  if (millis() - lastDhtRead < DHT_READ_INTERVAL && !isnan(cachedDhtTemp)) return;
  lastDhtRead = millis();
  float t, h;
  readDhtBoth(&t, &h);
}

float readPressureSafe() {
  if (!bmpAvailable) return lastValidPressureKpa;
  float p = bmp.readPressure() / 1000.0f;
  if (isnan(p) || p < PRESS_MIN_KPA || p > PRESS_MAX_KPA) {
    pressureErrorCount++;
    return lastValidPressureKpa;
  }
  lastValidPressureKpa = p;
  return p;
}

// 温度源模式：0=自动，1=强制BMP，2=强制DHT
uint8_t tempSourceMode = 0;

// BMP 与 DHT 双传感器对比 + 模式选择
float readTemperatureSafe() {
  // 先触发 DHT 读取（带缓存）
  updateDhtCache();
  float tDht = cachedDhtTemp;

  // 读 BMP
  float tBmp = NAN;
  if (bmpAvailable) {
    tBmp = bmp.readTemperature();
    if (isnan(tBmp) || tBmp < TEMP_MIN_C || tBmp > TEMP_MAX_C) {
      temperatureErrorCount++;
      tBmp = NAN;
    }
  }

  // ===== 强制 DHT =====
  if (tempSourceMode == 2 && !isnan(tDht)) {
    lastValidTempC = tDht;
    return tDht;
  }

  // ===== 强制 BMP =====
  if (tempSourceMode == 1 && !isnan(tBmp)) {
    lastValidTempC = tBmp;
    return tBmp;
  }

  // ===== 自动模式（原逻辑） =====
  if (!isnan(tBmp) && !isnan(tDht)) {
    float diff = fabs(tBmp - tDht);
    if (diff > TEMP_DIFF_THRESHOLD) {
      Serial.print("[TEMP] BMP=");
      Serial.print(tBmp, 1);
      Serial.print(" DHT=");
      Serial.print(tDht, 1);
      Serial.print(" diff=");
      Serial.print(diff, 1);
      Serial.println(" → DHT");
      lastValidTempC = tDht;
      return tDht;
    }
    lastValidTempC = tBmp;
    return tBmp;
  } else if (!isnan(tBmp)) {
    lastValidTempC = tBmp;
    return tBmp;
  } else if (!isnan(tDht)) {
    lastValidTempC = tDht;
    return tDht;
  }
  return lastValidTempC;
}

float readHumiditySafe() {
  updateDhtCache();
  if (isnan(cachedDhtHum)) return lastValidHum;
  lastValidHum = cachedDhtHum;
  return cachedDhtHum;
}
// ==================================================

// ==================== EEPROM 存储 ====================
#define EEPROM_SIZE 8192
#define EEPROM_ADDR_RECORD_COUNT 7990U
#define EEPROM_ADDR_INTERVAL     8000U
#define EEPROM_ADDR_WRITE_PTR    7980U
#define EEPROM_ADDR_BASE_TS      7974U
#define EEPROM_ADDR_TEMP_MODE    7968U

#define MAX_RECORDS 890
#define RECORD_BITS 73

unsigned int recordWritePtr = 0;
uint16_t recordCount = 0;
unsigned long sampleInterval = 600000UL;
unsigned long lastSampleTick = 0;

uint32_t baseTimestamp = 0;
bool baseTimestampSet = false;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 8*3600);
String bootTimeStr = "未获取时间";
bool ntpOk = false;

uint32_t getUnixTime() {
  timeClient.update();
  if (timeClient.isTimeSet()) return (uint32_t)timeClient.getEpochTime();
  return 0;
}

String formatTimestamp(uint32_t ts) {
  if (ts == 0) return "N/A";
  time_t t = (time_t)ts;
  struct tm* tm_info = gmtime(&t);
  if (!tm_info) return "N/A";
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
  return String(buf);
}

float calcDewPoint(float T, float RH) {
  if (isnan(T) || isnan(RH) || RH < 0 || RH > 100) return -999.0f;
  float es = 0.61078 * exp((17.27 * T) / (T + 237.3));
  float e = es * RH / 100.0f;
  return (237.3 * log(e / 0.61078)) / (17.27 - log(e / 0.61078));
}

float calcAppTemp(float T, float RH, float wind) {
  if (isnan(T) || isnan(RH) || isnan(wind) || RH < 0 || RH > 100) return -999.0f;
  float e = RH / 100.0 * 6.105 * exp(17.27 * T / (237.3 + T));
  return T + 0.33 * e - 0.70 * wind - 4.00;
}

float calcWetBulb(float T, float RH, float pressKpa) {
  if (isnan(T) || isnan(RH) || isnan(pressKpa)) return -999.0f;
  if (RH < 0 || RH > 100) return -999.0f;
  float tw = T * atan(0.151977f * sqrt(RH + 8.313659f))
           + atan(T + RH)
           - atan(RH - 1.676331f)
           + 0.00391838f * sqrt(RH * RH * RH) * atan(0.023101f * RH)
           - 4.686035f;
  if (isnan(tw)) return -999;
  return tw;
}

uint16_t loadRecordCountFromEEPROM() {
  uint16_t cnt;
  EEPROM.get(EEPROM_ADDR_RECORD_COUNT, cnt);
  if (cnt > MAX_RECORDS) cnt = MAX_RECORDS;
  return cnt;
}
void saveRecordCountToEEPROM(uint16_t cnt) {
  EEPROM.put(EEPROM_ADDR_RECORD_COUNT, cnt);
}
unsigned int loadWritePtrFromEEPROM() {
  unsigned int ptr;
  EEPROM.get(EEPROM_ADDR_WRITE_PTR, ptr);
  if (ptr >= MAX_RECORDS) ptr = 0;
  return ptr;
}
void saveWritePtrToEEPROM(unsigned int ptr) {
  EEPROM.put(EEPROM_ADDR_WRITE_PTR, ptr);
}
uint32_t loadBaseTimestampFromEEPROM() {
  uint32_t ts;
  EEPROM.get(EEPROM_ADDR_BASE_TS, ts);
  if (ts > 2000000000UL) ts = 0;
  return ts;
}
void saveBaseTimestampToEEPROM(uint32_t ts) {
  EEPROM.put(EEPROM_ADDR_BASE_TS, ts);
}
unsigned long loadIntervalFromEEPROM() {
  unsigned long val;
  EEPROM.get(EEPROM_ADDR_INTERVAL, val);
  if (val < 60000UL || val > 3600000UL) return 600000UL;
  return val;
}
void saveIntervalToEEPROM(unsigned long iv) {
  EEPROM.put(EEPROM_ADDR_INTERVAL, iv);
}
uint8_t loadTempModeFromEEPROM() {
  uint8_t m;
  EEPROM.get(EEPROM_ADDR_TEMP_MODE, m);
  if (m > 2) m = 0;
  return m;
}
void saveTempModeToEEPROM(uint8_t m) {
  EEPROM.put(EEPROM_ADDR_TEMP_MODE, m);
}

void bitStreamWrite(uint32_t eepromBase, uint32_t value, uint32_t bitLen) {
  uint32_t bitPos = eepromBase;
  for (uint32_t b = 0; b < bitLen; b++) {
    uint32_t byteIdx = bitPos / 8U;
    uint8_t bitInByte = bitPos % 8U;
    uint8_t buf;
    EEPROM.get(byteIdx, buf);
    if ((value >> b) & 1U) buf |= (1U << bitInByte);
    else buf &= ~(1U << bitInByte);
    EEPROM.put(byteIdx, buf);
    bitPos++;
  }
}
uint32_t bitStreamRead(uint32_t eepromBase, uint32_t bitLen) {
  uint32_t bitPos = eepromBase;
  uint32_t out = 0;
  for (uint32_t b = 0; b < bitLen; b++) {
    uint32_t byteIdx = bitPos / 8U;
    uint8_t bitInByte = bitPos % 8U;
    uint8_t buf;
    EEPROM.get(byteIdx, buf);
    if (buf & (1U << bitInByte)) out |= (1U << b);
    bitPos++;
  }
  return out;
}

bool saveWeatherRecord(float temp, float hum, float pressKpa, float wind, float dir, float rainDelta, uint32_t timestamp) {
  uint32_t baseBit = recordWritePtr * RECORD_BITS;
  int16_t tEnc = constrain((temp + 10.0f) * 10.0f, 0, 511);
  bitStreamWrite(baseBit, tEnc, 9); baseBit += 9;
  uint32_t hEnc = constrain(hum, 0, 100);
  bitStreamWrite(baseBit, hEnc, 7); baseBit += 7;
  uint32_t pEnc = constrain(pressKpa * 100.0f, 0, 16383);
  bitStreamWrite(baseBit, pEnc, 14); baseBit += 14;
  uint32_t wEnc = constrain(wind * 10.0f, 0, 255);
  bitStreamWrite(baseBit, wEnc, 8); baseBit += 8;
  uint32_t dEnc = constrain(dir, 0, 359);
  bitStreamWrite(baseBit, dEnc, 9); baseBit += 9;
  uint32_t rEnc = constrain(rainDelta * 100.0f, 0, 4095);
  bitStreamWrite(baseBit, rEnc, 12); baseBit += 12;
  uint32_t offsetMin = 0;
  if (baseTimestamp > 0 && timestamp >= baseTimestamp) {
    offsetMin = (timestamp - baseTimestamp) / 60;
    if (offsetMin > 16383) offsetMin = 16383;
  }
  bitStreamWrite(baseBit, offsetMin, 14);

  recordWritePtr = (recordWritePtr + 1) % MAX_RECORDS;
  saveWritePtrToEEPROM(recordWritePtr);
  if (recordCount < MAX_RECORDS) {
    recordCount++;
    saveRecordCountToEEPROM(recordCount);
  }
  return true;
}

bool readWeatherRecord(uint32_t idx, float *temp, float *hum, float *pressKpa,
                       float *wind, float *dir, float *rainDelta, uint32_t *timestamp) {
  uint32_t baseBit = idx * RECORD_BITS;
  uint32_t raw;
  raw = bitStreamRead(baseBit, 9); *temp = (raw / 10.0f) - 10.0f; baseBit += 9;
  raw = bitStreamRead(baseBit, 7); *hum = raw; baseBit += 7;
  raw = bitStreamRead(baseBit, 14); *pressKpa = raw / 100.0f; baseBit += 14;
  raw = bitStreamRead(baseBit, 8); *wind = raw / 10.0f; baseBit += 8;
  raw = bitStreamRead(baseBit, 9); *dir = raw; baseBit += 9;
  raw = bitStreamRead(baseBit, 12); *rainDelta = raw / 100.0f; baseBit += 12;
  raw = bitStreamRead(baseBit, 14);
  *timestamp = baseTimestamp + raw * 60;
  return true;
}

void clearAllRecords() {
  for (unsigned int i = 0; i < EEPROM_SIZE; i++) EEPROM.put(i, 0x00);
  recordWritePtr = 0;
  recordCount = 0;
  baseTimestamp = 0;
  baseTimestampSet = false;
  saveRecordCountToEEPROM(recordCount);
  saveWritePtrToEEPROM(recordWritePtr);
  saveBaseTimestampToEEPROM(baseTimestamp);
}
// ==================================================

// ==================== Web 页面 ====================
void sendWebPage(WiFiClient &client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type:text/html;charset=utf-8"));
  client.println(F("Connection:close"));
  client.println();
  client.println(F("<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width, initial-scale=1.0'><title>气象站</title>"));
  client.println(F("<style>*{box-sizing:border-box;margin:0;padding:0;}body{padding:16px;font-family:'Microsoft Yahei',system-ui,sans-serif;background:#0f172a;color:#e2e8f0;}h2{margin-bottom:18px;text-align:center;color:#f1f5f9;}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-bottom:20px;}.card{border:1px solid #334155;border-radius:14px;padding:16px;background:#1e293b;transition:0.2s;}.card:hover{border-color:#475569;}.card-label{font-size:14px;color:#94a3b8;margin-bottom:6px;}.card-value{font-size:26px;font-weight:bold;}.card-unit{font-size:14px;color:#94a3b8;margin-left:4px;}.alert-hot{border-color:#ef4444;}.alert-wet{border-color:#3b82f6;}.alert-rain{border-color:#06b6d4;}.box{border:1px solid #334155;border-radius:14px;padding:18px;margin-bottom:16px;background:#1e293b;}.box-title{font-size:16px;margin-bottom:12px;color:#cbd5e1;}button{padding:10px 18px;margin:4px;border:none;border-radius:10px;color:#fff;background:#2563eb;cursor:pointer;transition:0.15s;}button:hover{background:#1d4ed8;}button.danger{background:#dc2626;}button.danger:hover{background:#b91c1c;}input[type=range]{width:100%;margin:10px 0;}.range-info{display:flex;justify-content:space-between;font-size:14px;color:#94a3b8;}.bootline{margin-bottom:14px;color:#94a3b8;}.hist-wrap{max-height:320px;overflow-y:auto;border:1px solid #334155;border-radius:10px;}table{width:100%;border-collapse:collapse;font-size:12.5px;}th{position:sticky;top:0;background:#1e293b;color:#94a3b8;padding:8px 4px;text-align:center;border-bottom:1px solid #334155;}td{padding:6px 4px;text-align:center;border-bottom:1px solid #273244;}tr:hover{background:#273244;}</style></head><body>"));
  client.println(F("<h2>气象站</h2><div class='bootline'>开机时间：<span id='bt'>--</span></div><div class='grid'>"));
  client.println(F("<div class='card' id='card_temp'><div class='card-label'>温度</div><div class='card-value'><span id='t'>--</span><span class='card-unit'>℃</span></div></div>"));
  client.println(F("<div class='card' id='card_hum'><div class='card-label'>湿度</div><div class='card-value'><span id='h'>--</span><span class='card-unit'>%RH</span></div></div>"));
  client.println(F("<div class='card'><div class='card-label'>气压</div><div class='card-value'><span id='p'>--</span><span class='card-unit'>kPa</span></div></div>"));
  client.println(F("<div class='card'><div class='card-label'>极大风速</div><div class='card-value'><span id='ws'>--</span><span class='card-unit'>m/s</span></div></div>"));
  client.println(F("<div class='card'><div class='card-label'>风向</div><div class='card-value'><span id='wd'>--</span><span class='card-unit'>°</span></div></div>"));
  client.println(F("<div class='card' id='card_rain'><div class='card-label'>本周期增量雨量</div><div class='card-value'><span id='r'>--</span><span class='card-unit'>mm</span></div></div>"));
  client.println(F("<div class='card'><div class='card-label'>露点温度</div><div class='card-value'><span id='dp'>--</span><span class='card-unit'>℃</span></div></div>"));
  client.println(F("<div class='card'><div class='card-label'>体感温度</div><div class='card-value'><span id='at'>--</span><span class='card-unit'>℃</span></div></div>"));
  client.println(F("<div class='card'><div class='card-label'>湿球温度</div><div class='card-value'><span id='wb'>--</span><span class='card-unit'>℃</span></div></div></div>"));

  client.println(F("<div class='box'><div class='box-title'>采集间隔设置</div><div class='range-info'><span>1分钟</span><span id='iv_text'>-- 分钟</span><span>60分钟</span></div><input type='range' min='60000' max='3600000' step='60000' id='rng'><button onclick='applyInterval()'>应用间隔</button></div>"));

  // 温度源切换
  client.println(F("<div class='box'><div class='box-title'>温度源切换</div><button id='btn_auto' onclick=\"setTempMode('auto')\">自动</button><button id='btn_bmp' onclick=\"setTempMode('bmp')\">强制 BMP</button><button id='btn_dht' onclick=\"setTempMode('dht')\">强制 DHT</button><span id='temp_mode_text' style='margin-left:12px;color:#94a3b8;'></span></div>"));

  client.println(F("<div class='box'><button onclick=\"window.open('/csv')\">下载CSV(旧→新)</button><button class='danger' onclick=\"if(confirm('确定清空全部历史记录?')) fetch('/clear')\">清空历史</button></div>"));
  client.println(F("<div class='box'><div class='box-title'>历史记录（实时刷新）</div><div class='hist-wrap'><table id='histTable'><thead><tr><th>时间</th><th>温度</th><th>湿度</th><th>气压</th><th>极大风速</th><th>风向</th><th>雨量</th><th>露点</th><th>体感</th><th>湿球</th></tr></thead><tbody id='histBody'></tbody></table></div></div>"));

  client.println(F("<script>let refreshTimer;let histTimer;function updateStatus(d){document.getElementById('card_temp').className='card';document.getElementById('card_hum').className='card';document.getElementById('card_rain').className='card';if(d.temp>34)document.getElementById('card_temp').classList.add('alert-hot');if(d.hum>85)document.getElementById('card_hum').classList.add('alert-wet');if(d.rain>0.05)document.getElementById('card_rain').classList.add('alert-rain');}async function refreshData(){try{const res=await fetch('/api');if(!res.ok)throw 'http error';const d=await res.json();document.getElementById('bt').innerText=d.boot;document.getElementById('t').innerText=d.temp.toFixed(1);document.getElementById('h').innerText=d.hum.toFixed(0);document.getElementById('p').innerText=d.press.toFixed(2);document.getElementById('ws').innerText=d.wind.toFixed(2);document.getElementById('wd').innerText=d.dir.toFixed(0);document.getElementById('r').innerText=d.rain.toFixed(2);document.getElementById('dp').innerText=d.dew.toFixed(1);document.getElementById('at').innerText=d.appt.toFixed(1);document.getElementById('wb').innerText=d.wetb.toFixed(1);document.getElementById('iv_text').innerText=(d.interval/60000).toFixed(0)+' 分钟';document.getElementById('rng').value=d.interval;updateStatus(d);updateTempModeUI(d.tempmode);}catch(e){console.error(e);}}async function refreshHistory(){try{const resp=await fetch('/csv');if(!resp.ok)return;const csvText=await resp.text();const lines=csvText.trim().split('\\n');const tbody=document.getElementById('histBody');tbody.innerHTML='';const showMax=35;let startIdx=lines.length>showMax?lines.length-showMax:0;for(let i=startIdx;i<lines.length;i++){const cols=lines[i].split(',');const tr=document.createElement('tr');cols.forEach(c=>{const td=document.createElement('td');td.innerText=c.trim();tr.appendChild(td);});tbody.appendChild(tr);}}catch(err){console.error(err);}}document.getElementById('rng').addEventListener('input',function(){let m=this.value/60000;document.getElementById('iv_text').innerText=m.toFixed(0)+' 分钟';});async function applyInterval(){const v=document.getElementById('rng').value;await fetch('/set?i='+v);refreshData();}async function setTempMode(m){await fetch('/settemp?mode='+m);refreshData();}function updateTempModeUI(mode){const btnA=document.getElementById('btn_auto');const btnB=document.getElementById('btn_bmp');const btnD=document.getElementById('btn_dht');const txt=document.getElementById('temp_mode_text');btnA.style.background='#2563eb';btnB.style.background='#2563eb';btnD.style.background='#2563eb';if(mode===1){btnB.style.background='#16a34a';txt.innerText='当前：强制 BMP';}else if(mode===2){btnD.style.background='#16a34a';txt.innerText='当前：强制 DHT';}else{btnA.style.background='#16a34a';txt.innerText='当前：自动';}}refreshData();refreshHistory();refreshTimer=setInterval(refreshData,30000);histTimer=setInterval(refreshHistory,45000);</script></body></html>"));
}

// ==================== setup ====================
void setup() {
  Serial.begin(9600);
  delay(1500);

  Wire.begin();
  Wire.setClock(10000);
  bool bmpOk = false;
  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.print("[BMP280] I2C 初始化尝试 ");
    Serial.print(attempt);
    Serial.print("/5 ... ");
    if (bmp.begin(BMP280_ADDR)) {
      bmpOk = true;
      Serial.println("成功");
      break;
    }
    Serial.println("失败");
    delay(500);
  }

  if (!bmpOk) {
    Serial.println("[BMP280] I2C 初始化失败，温度回退 DHT11，气压用上次有效值");
    bmpAvailable = false;
  } else {
    bmpAvailable = true;
    float t0 = bmp.readTemperature();
    float p0 = bmp.readPressure() / 1000.0f;
    if (!isnan(t0)) lastValidTempC = t0;
    if (!isnan(p0)) lastValidPressureKpa = p0;
    Serial.print("[BMP280] 初始气压: ");
    Serial.print(lastValidPressureKpa, 2);
    Serial.print(" kPa | 初始温度: ");
    Serial.print(lastValidTempC, 2);
    Serial.println(" C");
  }

  scale.begin(HX711_DT, HX711_SCK);
  scale.set_scale(scaleFactor);
  tareOffset = scale.get_units(10);
  lastWaterGram = 0;
  deltaRainMM = 0;

  pinMode(WIND_PIN, INPUT);
  pinMode(WIND_DIR_PIN, INPUT);

  dht.begin();

  Serial.print("Connecting "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP:"); Serial.println(WiFi.localIP());

  timeClient.begin();
  bootTimeStr = "未获取时间";
  for (int i = 1; i <= 3; ++i) {
    timeClient.update();
    if (timeClient.isTimeSet()) {
      bootTimeStr = timeClient.getFormattedTime();
      ntpOk = true;
      break;
    }
    delay(300);
  }

  sampleInterval = loadIntervalFromEEPROM();
  recordCount = loadRecordCountFromEEPROM();
  recordWritePtr = loadWritePtrFromEEPROM();
  baseTimestamp = loadBaseTimestampFromEEPROM();
  baseTimestampSet = (baseTimestamp > 0);
  tempSourceMode = loadTempModeFromEEPROM();

  lastSampleTick = millis();
  lastWindSample = millis();
  webServer.begin();
}

// ==================== loop ====================
void loop() {
  if (millis() - lastWindSample >= WIND_SAMPLE_MS) {
    lastWindSample = millis();
    float w = analogRead(WIND_PIN) / 1023.0f * 10.0f;
    if (w > windMaxInInterval) windMaxInInterval = w;
  }

  WiFiClient client = webServer.available();
  if (client) {
    String req = client.readStringUntil('\n');
    client.flush();

    if (req.startsWith("GET / ")) {
      sendWebPage(client);
    }
    else if (req.startsWith("GET /api")) {
      float temp = readTemperatureSafe();
      float hum = readHumiditySafe();
      float pressKpa = readPressureSafe();
      float dir = analogRead(WIND_DIR_PIN) / 1023.0f * 360.0f;
      float dew = calcDewPoint(temp, hum);
      float appt = calcAppTemp(temp, hum, windMaxInInterval);
      float wetb = calcWetBulb(temp, hum, pressKpa);

      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type:application/json;charset=utf-8"));
      client.println(F("Connection:close"));
      client.println();
      client.print("{\"boot\":\"");
      client.print(bootTimeStr);
      client.print("\",\"temp\":"); client.print(temp, 1);
      client.print(",\"hum\":"); client.print(hum, 0);
      client.print(",\"press\":"); client.print(pressKpa, 2);
      client.print(",\"wind\":"); client.print(windMaxInInterval, 2);
      client.print(",\"dir\":"); client.print(dir, 0);
      client.print(",\"rain\":"); client.print(deltaRainMM, 2);
      client.print(",\"dew\":"); client.print(dew, 1);
      client.print(",\"appt\":"); client.print(appt, 1);
      client.print(",\"wetb\":"); client.print(wetb, 1);
      client.print(",\"interval\":"); client.print(sampleInterval);
      client.print(",\"tempmode\":"); client.print(tempSourceMode);
      client.println("}");
    }
    else if (req.startsWith("GET /set?i=")) {
      int pos = req.indexOf('=');
      unsigned long newIv = req.substring(pos + 1).toInt();
      if (newIv >= 60000UL && newIv <= 3600000UL) {
        sampleInterval = newIv;
        saveIntervalToEEPROM(sampleInterval);
      }
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type:text/plain"));
      client.println(F("Connection:close"));
      client.println();
      client.print(sampleInterval);
    }
    else if (req.startsWith("GET /settemp?mode=")) {
      int pos = req.indexOf('=');
      String modeStr = req.substring(pos + 1);
      modeStr.trim();
      if (modeStr.startsWith("bmp"))       tempSourceMode = 1;
      else if (modeStr.startsWith("dht"))  tempSourceMode = 2;
      else                                  tempSourceMode = 0;
      saveTempModeToEEPROM(tempSourceMode);

      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type:text/plain"));
      client.println(F("Connection:close"));
      client.println();
      client.print(tempSourceMode);
    }
    else if (req.startsWith("GET /csv")) {
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type:text/csv;charset=utf-8"));
      client.println(F("Connection:close"));
      client.println();

      float t, h, p, w, d, r;
      uint32_t ts;
      uint32_t start = (recordCount < MAX_RECORDS) ? 0 : recordWritePtr;
      for (uint32_t i = 0; i < recordCount; i++) {
        uint32_t idx = (start + i) % MAX_RECORDS;
        readWeatherRecord(idx, &t, &h, &p, &w, &d, &r, &ts);
        float dew = calcDewPoint(t, h);
        float appt = calcAppTemp(t, h, w);
        float wetb = calcWetBulb(t, h, p);

        client.print(formatTimestamp(ts)); client.print(",");
        client.print(t, 1); client.print(",");
        client.print(h, 0); client.print(",");
        client.print(p, 2); client.print(",");
        client.print(w, 2); client.print(",");
        client.print(d, 0); client.print(",");
        client.print(r, 2); client.print(",");
        client.print(dew, 1); client.print(",");
        client.print(appt, 1); client.print(",");
        client.println(wetb, 1);
        if (!client.connected()) break;
      }
    }
    else if (req.startsWith("GET /clear")) {
      clearAllRecords();
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type:text/plain"));
      client.println(F("Connection:close"));
      client.println();
      client.print("ok");
    }
    else {
      client.println(F("HTTP/1.1 404 Not Found"));
      client.println(F("Connection:close"));
      client.println();
    }
    client.stop();
  }

  if (scale.is_ready()) {
    float rawGram = scale.get_units(5);
    float currentGram = rawGram - tareOffset;
    if (currentGram < 0) currentGram = 0;
    float areaCM2 = PI * (COLLECTOR_MM / 10.0f / 2.0f) * (COLLECTOR_MM / 10.0f / 2.0f);
    float currentRainMM = currentGram / areaCM2 * 10.0f;
    deltaRainMM = currentRainMM - lastWaterGram / areaCM2 * 10.0f;
    if (deltaRainMM < 0) deltaRainMM = 0;
    if (deltaRainMM < RAIN_DELTA_THRESHOLD) deltaRainMM = 0.0f;
  }

  if (millis() - lastSampleTick >= sampleInterval) {
    float rawGram = scale.get_units(5);
    lastWaterGram = rawGram - tareOffset;
    if (lastWaterGram < 0) lastWaterGram = 0;

    lastSampleTick = millis();

    float temp = readTemperatureSafe();
    float hum = readHumiditySafe();
    float pressKpa = readPressureSafe();
    float dir = analogRead(WIND_DIR_PIN) / 1023.0f * 360.0f;

    uint32_t now = getUnixTime();

    if (!baseTimestampSet && now > 0) {
      baseTimestamp = now;
      baseTimestampSet = true;
      saveBaseTimestampToEEPROM(baseTimestamp);
      Serial.print("[TIME] baseTimestamp 已设置: ");
      Serial.println(formatTimestamp(baseTimestamp));
    }

    saveWeatherRecord(temp, hum, pressKpa, windMaxInInterval, dir, deltaRainMM, now);

    Serial.print("[SAMPLE] ");
    Serial.print(formatTimestamp(now));
    Serial.print(" T="); Serial.print(temp, 1);
    Serial.print(" H="); Serial.print(hum, 0);
    Serial.print(" P="); Serial.print(pressKpa, 2);
    Serial.print(" Wmax="); Serial.print(windMaxInInterval, 2);
    Serial.print(" R="); Serial.println(deltaRainMM, 2);

    windMaxInInterval = 0.0f;
    deltaRainMM = 0.0f;
  }
}