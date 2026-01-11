#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <WiFi.h>
#include <time.h>

#define DHTPIN 4
#define DHTTYPE DHT22

#define PRESSURE_OUT 19
#define PRESSURE_SCK 18

#define POT_PIN 35
#define LDR_PIN 34
#define SUN_THRESHOLD 2000

#define SDA_PIN 21
#define SCL_PIN 22

#define GMT_OFFSET_SEC   (6 * 3600)
#define DAYLIGHT_OFFSET  0

// ---- Wifi Data ----
const char* ntpServer = "pool.ntp.org";

const char* ssid = "Asif's iPhone";
const char* password = "hasauiphone";

// ---- LDR to Sunshine Variables ----
unsigned long lastSample = 0;
const unsigned long sampleInterval = 5000; // 5 seconds

unsigned long sunshineMillis = 0;
float sunshineHours = 0;

// ---- Time-dependent weather storage ----
float temperature = NAN;
float humidity  = NAN;
float temp9am = NAN;
float temp3pm = NAN;
float humidity9am = NAN;
float humidity3pm = NAN;
float pressure9am = NAN;
float pressure3pm = NAN;
float pressure_kPa=NAN;
int pressureTrend=-1;
float dayCos, monthSin, monthCos;
float prob = -1;
int pred = -1;


/* ===== Readable raw inputs and variable declarance ===== */

float minTemp = 100, maxTemp = -10;
int windGustSpeed = -1;
int windSpeed = NAN, windSpeed9am = NAN, windSpeed3pm = NAN;

float avgTemp = (temp9am + temp3pm) / 2.0; 
float tempRange = maxTemp - minTemp, avgHumidity = (humidity9am + humidity3pm) / 2.0;


int gustDir = 13;        // W
int wind9am = 13;        // W
int wind3pm = 10;        // SSE


bool temp9amCaptured = false;
bool temp3pmCaptured = false;

// ---- Necessary Functions ----

void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void initTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }

  Serial.println("Time initialized");
}
void getTimeFeatures(float &dayCos, float &monthSin, float &monthCos) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int dayOfYear = timeinfo.tm_yday + 1;   // 1–365
  int month = timeinfo.tm_mon + 1;        // 1–12

  dayCos   = cos(2 * PI * dayOfYear / 365.0);
  monthSin = sin(2 * PI * month / 12.0);
  monthCos = cos(2 * PI * month / 12.0);
}


/* ===================== OBJECTS ===================== */
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

/* ===================== PRESSURE READ FUNCTION ===================== */
// Generic 24-bit clocked sensor read
long readPressureRaw() {
  long value = 0;

  while (digitalRead(PRESSURE_OUT) == HIGH); // wait until ready

  for (int i = 0; i < 24; i++) {
    digitalWrite(PRESSURE_SCK, HIGH);
    delayMicroseconds(1);
    value = value << 1;
    digitalWrite(PRESSURE_SCK, LOW);
    delayMicroseconds(1);
    if (digitalRead(PRESSURE_OUT)) {
      value++;
    }
  }

  // extra clock pulse
  digitalWrite(PRESSURE_SCK, HIGH);
  delayMicroseconds(1);
  digitalWrite(PRESSURE_SCK, LOW);

  return value;
}

/* ===== Model configuration ===== */
#define N_FEATURES 67
#define THRESHOLD 0.47

/* ===== Logistic Regression Parameters ===== */
const float weights[N_FEATURES] = {
  0.07362754, -0.08981350,  0.01086753, -0.00217508, -0.61792424,
  0.29643481,  0.12007842,  0.01490530,  0.15414372,  0.37984357,
 -0.00409594, -0.01895492,  0.30093570, -0.00996000,  0.02237808,
 -0.01256643,  0.02800310, -0.11664450,  0.02291629, -0.00655218,
 -0.08461577, -0.05837901,  0.01738162,  0.02282338, -0.00447027,
  0.02697126,  0.04651429,  0.10061521,  0.00979295, -0.01300217,
  0.00913659, -0.00063915,  0.01649260,  0.01838207, -0.00418835,
 -0.01652992, -0.05393560, -0.02850435,  0.00388143,  0.05215860,
  0.01010113,  0.01815742,  0.02939231,  0.03623460,  0.00658478,
 -0.02219399, -0.05784941, -0.00833166, -0.14549521, -0.08769155,
  0.02146041, -0.00529291, -0.05978142,  0.04927729,  0.03735164,
  0.00611680,  0.09175884,  0.02900284,  0.00885173,  0.08939068,
  0.02349516, -0.03906453, -0.02384809,  0.00431997, -0.01047961,
  0.02889728, -0.01755957
};

const float bias = 0.06140133;

/* ===== Feature indices ===== */
enum FeatureIndex {
  Temp9am, Temp3pm, MinTemp, MaxTemp, Sunshine, WindGustSpeed,
  WindSpeed9am, WindSpeed3pm, Humidity9am, Humidity3pm,
  AvgTemp, TempRange, AvgHumidity,
  PressureTrend_Falling, PressureTrend_Rising, PressureTrend_Stable,
  WindGustDir_E, WindGustDir_ENE, WindGustDir_ESE, WindGustDir_N,
  WindGustDir_NE, WindGustDir_NNE, WindGustDir_NNW, WindGustDir_NW,
  WindGustDir_S, WindGustDir_SE, WindGustDir_SSE, WindGustDir_SSW,
  WindGustDir_SW, WindGustDir_W, WindGustDir_WNW, WindGustDir_WSW,
  WindDir9am_E, WindDir9am_ENE, WindDir9am_ESE, WindDir9am_N,
  WindDir9am_NE, WindDir9am_NNE, WindDir9am_NNW, WindDir9am_NW,
  WindDir9am_S, WindDir9am_SE, WindDir9am_SSE, WindDir9am_SSW,
  WindDir9am_SW, WindDir9am_W, WindDir9am_WNW, WindDir9am_WSW,
  WindDir3pm_E, WindDir3pm_ENE, WindDir3pm_ESE, WindDir3pm_N,
  WindDir3pm_NE, WindDir3pm_NNE, WindDir3pm_NNW, WindDir3pm_NW,
  WindDir3pm_S, WindDir3pm_SE, WindDir3pm_SSE, WindDir3pm_SSW,
  WindDir3pm_SW, WindDir3pm_W, WindDir3pm_WNW, WindDir3pm_WSW,
  DayOfYear_cos, Month_sin, Month_cos
};

/* ===== StandardScaler parameters ===== */
const float feature_mean[N_FEATURES] = {
  17.876380, 21.079397, 15.120162, 22.653790, 6.049963, 42.506255, 15.225901, 19.200883, 
  70.697572, 58.948492, 19.477888, 7.533628, 64.823032, 0.000000, 0.000000, 0.000000, 
  0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 
  0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 
  0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 
  0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 
  0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 
  0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 
  -0.025670, -0.043703, -0.019853,
};

const float feature_scale[N_FEATURES] = {
  4.887385, 4.411491, 4.572531, 4.601300, 4.054068, 11.254096, 7.156284, 7.650581, 
  15.244070, 17.293008, 4.439195, 3.149595, 14.817809, 1.000000, 1.000000, 1.000000, 
  1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 
  1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 
  1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 
  1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 
  1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 
  1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 
  0.686667, 0.723008, 0.689170,
};

/* ===== Math & prediction ===== */
float sigmoid(float x) {
  return (x >= 0) ? 1.0f / (1.0f + expf(-x)) : expf(x) / (1.0f + expf(x));
}

float predict_proba(const float *x) {
  float z = bias;
  for (int i = 0; i < N_FEATURES; i++) z += weights[i] * x[i];
  return sigmoid(z);
}

int predict(const float *x) {
  return (predict_proba(x) >= THRESHOLD) ? 1 : 0;
}

void scale_features(float *raw, float *scaled) {
  for (int i = 0; i < N_FEATURES; i++)
    scaled[i] = (raw[i] - feature_mean[i]) / feature_scale[i];
}

/* ===== One-hot helpers ===== */
void set_pressure_trend(float *x, int t) {
  x[PressureTrend_Falling] = (t == 0);
  x[PressureTrend_Rising]  = (t == 1);
  x[PressureTrend_Stable]  = (t == 2);
}

void set_wind_dir(float *x, int base, int dir) {
  for (int i = 0; i < 16; i++) x[base + i] = (i == dir);
}
float convertPressure_kPa(long raw) {
  // Convert raw 24-bit value to uncalibrated pressure
  float raw_kPa = (raw / 16777215.0) * 1000.0;

  // Calibration factor (derived from 101.3 / 134.6)
  const float CAL_FACTOR = 0.734;

  return raw_kPa * CAL_FACTOR;
}



/* ===== Arduino ===== */
void setup() {
  Serial.begin(115200);
  analogReadResolution(12); // 0–4095
  delay(1000);

  pinMode(PRESSURE_SCK, OUTPUT);
  pinMode(PRESSURE_OUT, INPUT);

  dht.begin();

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("ESP32 Sensors");
  delay(2000);
  lcd.clear();

  getTimeFeatures(dayCos, monthSin, monthCos);

  connectWiFi();
  initTime();
}

void loop() {
  // --- Final Boss ---
  if (temp3pmCaptured && pressureTrend != -1) {
    float raw[N_FEATURES] = {0};

    // Fill raw features
    raw[Temp9am] = temp9am;
    raw[Temp3pm] = temp3pm;
    raw[MinTemp] = minTemp;
    raw[MaxTemp] = maxTemp;
    raw[Sunshine] = sunshineHours;
    raw[WindGustSpeed] = windGustSpeed;
    raw[WindSpeed9am] = windSpeed9am;
    raw[WindSpeed3pm] = windSpeed3pm;
    raw[Humidity9am] = humidity9am;
    raw[Humidity3pm] = humidity3pm;
    raw[AvgTemp] = (temp9am + temp3pm) / 2.0;
    raw[TempRange] = maxTemp - minTemp;
    raw[AvgHumidity] = (humidity9am + humidity3pm) / 2.0;

    // One-hot / categorical features
    set_pressure_trend(raw, pressureTrend);
    set_wind_dir(raw, WindGustDir_E, gustDir);
    set_wind_dir(raw, WindDir9am_E, wind9am);
    set_wind_dir(raw, WindDir3pm_E, wind3pm);

    // Time features
    raw[DayOfYear_cos] = dayCos;
    raw[Month_sin] = monthSin;
    raw[Month_cos] = monthCos;

    // Scale and predict
    float scaled[N_FEATURES];
    scale_features(raw, scaled);

    prob = predict_proba(scaled);
    pred = predict(scaled);
    Serial.print("Prediction Done");

    // Reset flag so prediction runs only once
    temp3pmCaptured = false;
  }

  // --- Setting Pressure Trend based on Pressure drop ---
  if (!isnan(pressure9am) && !isnan(pressure3pm)) {
    float pressure_drop=pressure9am-pressure3pm;

    if(pressure_drop>0){
      pressureTrend = 0; 
    }
    else if(pressure_drop<0){
      pressureTrend = 1;
    }
    else if(pressure_drop==0){
      pressureTrend = 2; 
    }   // 0=Falling 1=Rising 2=Stable

  }

  // --- Conversion of LDR Readings to Hourly Sunshine Data ---
  unsigned long currentMillis = millis();

  if (currentMillis - lastSample >= sampleInterval) {
    lastSample = currentMillis;

    int ldrValue = analogRead(LDR_PIN);
    Serial.print("LDR: ");
    Serial.println(ldrValue);

    if (ldrValue > SUN_THRESHOLD) {
      sunshineMillis += sampleInterval;
    }
  }
  sunshineHours = sunshineMillis / 3600000.0;

  // --- DHT22 ---
  temperature = dht.readTemperature();
  if(temperature>maxTemp){
      maxTemp = temperature;
  }
  if(temperature>minTemp){
      minTemp = temperature;
  }
  humidity = dht.readHumidity();

  // --- Pressure Sensor ---
  long pressureRaw = readPressureRaw();
  pressure_kPa = convertPressure_kPa(pressureRaw);

  // ---- Data Storing & Resetting based on time ----
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {

    int hour = timeinfo.tm_hour;
    int minute = timeinfo.tm_min;

    // ---- Capture 9 AM data ----
    if (hour == 9 && minute == 0 && !temp9amCaptured) {
      temp9am = temperature;
      humidity9am = humidity;
      pressure9am = pressure_kPa;
      windSpeed9am = windSpeed;

      temp9amCaptured = true;

      Serial.println("✔ 9 AM data captured");
    }

    // ---- Capture 3 PM data ----
    if (hour == 15 && minute == 0 && !temp3pmCaptured) {
      temp3pm = temperature;
      humidity3pm = humidity;
      pressure3pm = pressure_kPa;
      windSpeed3pm = windSpeed;
      
      temp3pmCaptured = true;

      Serial.println("✔ 3 PM data captured");
    }

    // ---- Reset at midnight ----
    if (hour == 0 && minute == 0) {
      temp9amCaptured = false;
      temp3pmCaptured = false;//did not reset humidity or pressure
      minTemp = 100; 
      maxTemp = -10;
      sunshineMillis = 0;
      sunshineHours = 0;
      prob = -1;
      pred = -1;

      Serial.println("🔄 Daily reset");
    }
  }

  // --- Analog Sensors (LDR & Potentiometer)---
  int potValue = analogRead(POT_PIN);
  windSpeed = (potValue * 100) / 4095;
  if(windSpeed>windGustSpeed){
    windGustSpeed = windSpeed;
  }
  int ldrValue = analogRead(LDR_PIN);

  // --- SERIAL OUTPUT ---
  Serial.println("---- Raw Data ----");
  Serial.print("Temp: "); Serial.print(temperature); Serial.println(" C");
  Serial.print("Humidity: "); Serial.print(humidity); Serial.println(" %");
  Serial.print("Potentiometer: "); Serial.println(potValue);
  Serial.print("WindSpeed: "); Serial.println(windSpeed);
  Serial.print("LDR: "); Serial.println(ldrValue);
  Serial.print("Pressure RAW: "); Serial.println(pressureRaw);
  Serial.println(sunshineMillis);
  Serial.print("Sunshine hours today: ");
  Serial.println(sunshineHours, 2);

  Serial.println("---- Time-based values ----");
  Serial.print("Temp9am: "); Serial.println(temp9am);
  Serial.print("Temp3pm: "); Serial.println(temp3pm);
  Serial.print("Hum9am: "); Serial.println(humidity9am);
  Serial.print("Hum3pm: "); Serial.println(humidity3pm);
  
  Serial.print("Pressure9am: ");
  Serial.println(pressure9am, 5);
  Serial.print("Pressure3pm: ");
  Serial.println(pressure3pm, 5);
  Serial.print("PressureTrend: ");
  if (pressureTrend == 0) Serial.println("Falling");
  else if (pressureTrend == 1) Serial.println("Rising");
  else if (pressureTrend == 2) Serial.println("Stable");
  else Serial.println("Not calculated");
  
  if(prob != -1){
    Serial.println("---- Predictions ----");
    Serial.print("Probability: ");
    Serial.println(prob, 6);
    Serial.print("Prediction: ");
    Serial.println(pred);
  }

  // --- LCD DISPLAY ---
  lcd.clear();

  //Display Probability and prediction
  if(prob != -1){
    lcd.setCursor(0, 0);
    lcd.print("Prob:");
    lcd.setCursor(0, 1);
    lcd.print(prob, 2);
    delay(2000);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Rain?");
    lcd.setCursor(0, 1);
    if(pred == 1){
      lcd.print("Yes!");
    }
    else{
      lcd.print("No.");
    }
    delay(2000);
    
  }

  // Display Temperature
  lcd.setCursor(0, 0);
  lcd.print("Temp:");
  lcd.setCursor(0, 1);
  lcd.print(temperature, 1);
  lcd.print("C");
  delay(2000);

  // Display Humidity
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Humid:");
  lcd.setCursor(0, 1);
  lcd.print(humidity, 0);
  lcd.print("%");
  delay(2000);

  // Display Pressure
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Pres:");
  lcd.setCursor(0, 1);
  lcd.print(pressure_kPa, 1);
  lcd.print("kPa");
  delay(2000);
  
  // Display Time (NTP)
  if (getLocalTime(&timeinfo)) {
      // --- SERIAL OUTPUT ---
    Serial.println("---- Time and Date ----");
    Serial.print("Time: ");
    Serial.printf("%02d:%02d:%02d  ",
                  timeinfo.tm_hour,
                  timeinfo.tm_min,
                  timeinfo.tm_sec);

    Serial.printf("Date: %02d/%02d/%04d\n",
                  timeinfo.tm_mday,
                  timeinfo.tm_mon + 1,
                  timeinfo.tm_year + 1900);


    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("T:");
    lcd.setCursor(0, 1);
    lcd.printf("%02d:%02d:%02d",
              timeinfo.tm_hour,
              timeinfo.tm_min,
              timeinfo.tm_sec);
    delay(2000);
  }
}