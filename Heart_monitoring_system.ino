/*******************************************************
 * ESP32 Health Monitor with On-Device Tiny ML
 * Sensors: ECG (analog), MAX30102 (SpO2), DS18B20 (Temp)
 * Windowed features (N=10) -> Five tiny decision trees
 * Predictions: Fever, Hypoxemia, Tachycardia, Bradycardia, Abnormal_ECG
 *
 * Libraries:
 *  - SparkFun MAX3010x (MAX30105.h)
 *  - OneWire, DallasTemperature (for DS18B20)
 *
 * Model headers (exported by micromlgen):
 *  - model_Tachycardia.h
 *  - model_Fever.h
 *  - model_Hypoxemia.h
 *  - model_Bradycardia.h
 *  - model_Abnormal_ECG.h
 *******************************************************/

#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"

// If you have Protocentral/Sparkfun spo2 algorithm header, include it and use it.
// #include "spo2_algorithm.h"   // optional; not strictly required if you already have SpO2 from your library

// Temperature (choose DS18B20 or LM35):
#define USE_DS18B20   1
#if USE_DS18B20
  #include <OneWire.h>
  #include <DallasTemperature.h>
  #define TEMP_DQ_PIN 4
  OneWire oneWire(TEMP_DQ_PIN);
  DallasTemperature sensors(&oneWire);
#else
  // LM35 analog on GPIO 35 (adjust to your wiring)
  #define LM35_PIN 35
#endif

// ECG analog input (AD8232 OUT to GPIO 34)
#define ECG_PIN 34

// I2C pins for MAX30102 (use defaults: SDA 21, SCL 22)
MAX30105 particleSensor;

// ----------------- ML model headers ------------------
#include "model_Tachycardia.h"
#include "model_Fever.h"
#include "model_Hypoxemia.h"
#include "model_Bradycardia.h"
#include "model_Abnormal_ECG.h"

// Models generated by micromlgen typically expose:
//   int predict(const float *x);
// returning class index: 0 -> "NO", 1 -> "YES"
// Our feature vector length must match training export.
static const int N_FEATURES = 7;

// ------------------ Configuration --------------------
static const uint16_t SAMPLE_PERIOD_MS = 1000; // read sensors each second
static const int WINDOW = 10;                  // number of readings per window
static const int ECG_FS = 250;                 // ECG sampling Hz (simple peak detect)
static const float ECG_PEAK_THRESH = 0.6f;     // normalized threshold (tune)
static const uint16_t ECG_REFRACT_MS = 250;    // refractory period to avoid double peaks

// ------------------ State buffers --------------------
struct Reading {
  float hr_bpm;         // from ECG estimation (preferred)
  float temp_c;         // DS18B20 or LM35
  float spo2;           // from MAX30102 algorithm
  float rr_interval;    // seconds between R peaks
  int   ecg_abn;        // 0/1 abnormal ECG flag
  uint32_t ts_ms;
};

Reading ring[WINDOW];
int ringCount = 0;

// ECG peak detection state
uint32_t lastPeakMs = 0;
float lastRR = NAN;
float ecgBaseline = 0.0f;     // simple moving baseline
float ecgAlpha = 0.01f;       // smoothing factor
uint16_t ecgRefract = ECG_REFRACT_MS;
bool lastAbove = false;

// HR smoothing
float hrInst = NAN;  // instantaneous computed from RR
float hrEMA  = 75.0; // smoothed HR
float hrAlpha = 0.2f;

// Abnormal ECG counter for the window (will derive fraction)
int ecgAbnCounter = 0;

// ------------------ Helpers -------------------------
float readTemperatureC() {
#if USE_DS18B20
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  if (t < -20 || t > 80) return NAN;
  return t;
#else
  // LM35: 10 mV/°C. On ESP32 ADC ~ 3.3 V ref, 12-bit.
  int raw = analogRead(LM35_PIN); // 0..4095
  float v = (3.3f * raw) / 4095.0f;
  float tempC = v * 100.0f; // 0.01 V per °C
  if (tempC < 10 || tempC > 50) return NAN;  // crude sanity range
  return tempC;
#endif
}

// Simple ECG sampling for RR/HR using threshold-crossing peak detector
void sampleECGFor(uint16_t ms) {
  const uint32_t start = millis();
  const uint32_t sampleIntervalUs = 1000000UL / ECG_FS; // microseconds per sample
  uint32_t nextSample = micros();

  while (millis() - start < ms) {
    const uint32_t nowUs = micros();
    if ((int32_t)(nowUs - nextSample) >= 0) {
      nextSample += sampleIntervalUs;

      int raw = analogRead(ECG_PIN);   // 0..4095
      float x = raw / 4095.0f;         // normalize to 0..1
      // update baseline
      ecgBaseline = (1 - ecgAlpha) * ecgBaseline + ecgAlpha * x;
      float xc = x - ecgBaseline;      // AC component (rough)

      // detect threshold crossing from below
      bool above = xc > ECG_PEAK_THRESH;
      uint32_t nowMs = millis();

      if (above && !lastAbove) {
        // rising edge; check refractory
        if (nowMs - lastPeakMs > ecgRefract) {
          // Peak detected
          if (lastPeakMs != 0) {
            float rr = (nowMs - lastPeakMs) / 1000.0f; // seconds
            if (rr >= 0.3f && rr <= 2.5f) {
              lastRR = rr;
              float hr = 60.0f / rr;
              // smooth HR
              if (isnan(hrEMA)) hrEMA = hr;
              else hrEMA = (1 - hrAlpha) * hrEMA + hrAlpha * hr;
              hrInst = hr;

              // mark abnormal if adjacent RR difference too high
              static float prevRR = NAN;
              if (!isnan(prevRR) && fabs(rr - prevRR) > 0.20f) {
                ecgAbnCounter++;
              }
              prevRR = rr;
            }
          }
          lastPeakMs = nowMs;
        }
      }
      lastAbove = above;
    }
    // yield to WiFi/BLE stack if needed
    delayMicroseconds(50);
  }
}

// Initialize MAX30102; returns true if ok
bool initMAX30102() {
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    return false;
  }
  // Configure for HR/SpO2
  particleSensor.setup();           // default settings are a good start
  particleSensor.setPulseAmplitudeRed(0x24);
  particleSensor.setPulseAmplitudeIR(0x24);
  particleSensor.setPulseAmplitudeGreen(0); // off
  return true;
}

// Read SpO2; this is a placeholder using library averages.
// For production, integrate the spo2 algorithm you use with your MAX30102 library.
float readSpO2Percent() {
  // Try grabbing some samples and compute a simple proxy.
  // In real deployments, use the proper algorithm to compute SpO2.
  static float spo2EMA = 98.0f;
  long ir = particleSensor.getIR();
  long red = particleSensor.getRed();
  if (ir < 5000 || red < 5000) {
    // Weak signal; return NAN to ignore
    return NAN;
  }
  // Placeholder heuristic: keep prior value; user should replace with real calc
  // If you already have SpO2 from your library, just return it here.
  return spo2EMA;
}

// Build features for current window
void computeFeatures(float feats[N_FEATURES]) {
  // Compute statistics over ring[0..ringCount-1]
  float hr_sum = 0, hr_sq = 0;
  float rr_sum = 0, rr_sq = 0;
  float spo2_min = 101, spo2_sum = 0;
  float temp_max = -100;
  int n = ringCount;
  int ecg_abn_sum = 0;

  for (int i = 0; i < n; ++i) {
    float hr = ring[i].hr_bpm;
    float rr = ring[i].rr_interval;
    float spo2 = ring[i].spo2;
    float temp = ring[i].temp_c;
    int eabn = ring[i].ecg_abn;

    // Basic cleaning: skip impossible values
    if (hr > 30 && hr < 220) {
      hr_sum += hr;
      hr_sq  += hr * hr;
    } else {
      // If invalid, approximate with smoothed HR
      hr_sum += hrEMA;
      hr_sq  += hrEMA * hrEMA;
    }

    if (rr > 0.3f && rr < 2.5f) {
      rr_sum += rr;
      rr_sq  += rr * rr;
    }

    if (!isnan(spo2) && spo2 >= 60 && spo2 <= 100) {
      spo2_min = min(spo2_min, spo2);
      spo2_sum += spo2;
    }

    if (!isnan(temp)) {
      temp_max = max(temp_max, temp);
    }

    if (eabn) ecg_abn_sum++;
  }

  float hr_mean = hr_sum / n;
  float hr_var  = max(0.0f, (hr_sq / n) - (hr_mean * hr_mean));
  float hr_std  = sqrtf(hr_var);

  float rr_mean = rr_sum / n;
  float rr_var  = max(0.0f, (rr_sq / n) - (rr_mean * rr_mean));
  float rr_std  = sqrtf(rr_var);

  float spo2_mean = spo2_sum / n;
  if (spo2_min > 100) spo2_min = 100;   // clamp if no valid readings
  if (temp_max < -50) temp_max = NAN;   // no valid temps -> will be NaN (handle)

  float ecg_abn_frac = ((float)ecg_abn_sum) / n;

  // Fill feature vector (must match training order)
  feats[0] = hr_mean;       // HR_mean
  feats[1] = hr_std;        // HR_std
  feats[2] = rr_std;        // RR_std
  feats[3] = spo2_min;      // SpO2_min
  feats[4] = spo2_mean;     // SpO2_mean
  feats[5] = temp_max;      // Temp_max
  feats[6] = ecg_abn_frac;  // ECG_abn_frac
}

// Convenience: convert NaN to reasonable defaults
static inline float nz(float v, float defv) { return isnan(v) ? defv : v; }

// Add a reading to the window buffer
void addReading(const Reading& r) {
  if (ringCount < WINDOW) {
    ring[ringCount++] = r;
  } else {
    // slide left by one (or use circular index)
    for (int i = 1; i < WINDOW; ++i) ring[i-1] = ring[i];
    ring[WINDOW-1] = r;
  }
}

// ------------------ Setup & Loop ---------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  // ADC setup for ECG/LM35
  analogReadResolution(12);  // 0..4095
  analogSetAttenuation(ADC_11db); // allow 0..~3.3V range

#if USE_DS18B20
  sensors.begin();
#else
  pinMode(LM35_PIN, INPUT);
#endif

  Wire.begin(); // SDA=21, SCL=22
  bool ok = initMAX30102();
  if (!ok) {
    Serial.println("[WARN] MAX30102 not found or init failed. SpO2 will be NAN.");
  }

  Serial.println("[i] ESP32 Health Monitor started. Collecting data...");
}

uint32_t lastTick = 0;

void loop() {
  // 1) Sample ECG at 250 Hz for ~1000 ms and update HR/RR state
  sampleECGFor(SAMPLE_PERIOD_MS);

  // 2) Read temperature
  float tempC = readTemperatureC();

  // 3) Read SpO2 (replace with your library's real function if available)
  float spo2 = readSpO2Percent();

  // 4) Build the 1-second reading
  Reading r;
  r.hr_bpm = nz(hrEMA, 75.0f);
  r.temp_c = tempC;
  r.spo2 = spo2;                      // OK if NAN; feature builder guards
  r.rr_interval = nz(lastRR, 60.0f/ nz(hrEMA, 75.0f)); // fallback if RR NAN
  r.ecg_abn = (ecgAbnCounter > 0) ? 1 : 0;
  r.ts_ms = millis();

  // Reset per-second abnormal counter
  ecgAbnCounter = 0;

  // 5) Add to window
  addReading(r);

  // 6) If window is full, compute features and run ML
  if (ringCount >= WINDOW) {
    float x[N_FEATURES];
    computeFeatures(x);

    // Replace NaNs with neutral defaults (keep in sync with training ranges)
    for (int i = 0; i < N_FEATURES; ++i) {
      if (isnan(x[i])) {
        // sensible defaults:
        if (i == 0) x[i] = 75.0f;       // HR_mean
        else if (i == 1) x[i] = 5.0f;   // HR_std
        else if (i == 2) x[i] = 0.05f;  // RR_std
        else if (i == 3) x[i] = 98.0f;  // SpO2_min
        else if (i == 4) x[i] = 98.0f;  // SpO2_mean
        else if (i == 5) x[i] = 36.8f;  // Temp_max
        else if (i == 6) x[i] = 0.0f;   // ECG_abn_frac
      }
    }

    // 7) Call each tiny model (0=NO,1=YES)
    int y_tachy = predict_Tachycardia(x);     // from model_Tachycardia.h
    int y_fever = predict_Fever(x);
    int y_hypox = predict_Hypoxemia(x);
    int y_brady = predict_Bradycardia(x);
    int y_ecgab = predict_Abnormal_ECG(x);

    bool isGood = !(y_tachy || y_fever || y_hypox || y_brady || y_ecgab);

    // 8) Print result
    Serial.println();
    Serial.println("=== Prediction ===");
    Serial.print("Features: ");
    for (int i=0;i<N_FEATURES;++i){ Serial.print(x[i], 4); Serial.print(i<N_FEATURES-1?", ":""); }
    Serial.println();
    Serial.printf("Tachycardia:%d  Fever:%d  Hypoxemia:%d  Bradycardia:%d  Abnormal_ECG:%d\n",
                  y_tachy, y_fever, y_hypox, y_brady, y_ecgab);
    Serial.print("Overall: "); Serial.println(isGood ? "Good" : "Issue(s) detected");

    // Slide window by 5 (50% overlap) to update more frequently
    if (WINDOW > 1) {
      int keep = WINDOW / 2;
      for (int i = 0; i < keep; ++i) ring[i] = ring[i + (WINDOW - keep)];
      ringCount = keep;
    }
  }

  // The main loop period is ~1s due to sampleECGFor(SAMPLE_PERIOD_MS)
}

/*************** MODEL CALL ADAPTERS *******************
 * micromlgen exports typically look like:
 *   class DecisionTree { public: int predict(const float *x); };
 *   static DecisionTree clf;
 *   int predict(const float *x) { return clf.predict(x); }
 *
 * If your generated headers expose different symbols, adapt the
 * wrappers below accordingly.
 ********************************************************/
extern "C" {
  // Prototypes we expect each header to define:
  // int predict(const float *x);  // but names may clash across headers
  // To avoid clashes, we wrap each header and rename their predict.
  int predict_Tachycardia(const float *x);
  int predict_Fever(const float *x);
  int predict_Hypoxemia(const float *x);
  int predict_Bradycardia(const float *x);
  int predict_Abnormal_ECG(const float *x);
}
