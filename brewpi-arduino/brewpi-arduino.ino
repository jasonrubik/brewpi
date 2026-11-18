/*  Full standalone Arduino Uno brewing controller
    - 5-step default recipe (ramp->add grain->10min rest->1hr mash->30min sparge)
    - NTC vessel probe (A0)
    - LM35 HEX outlet probe (A1) used for failsafe
    - SSR PWM on pin 9
    - LEDs:
        pin13 Red = Heat ON (duty > 5%)
        pin12 Green = Mash OK (within +/-0.5 F)
    - Buttons (INPUT_PULLUP, active LOW):
        RESET = pin7, SKIP = pin8, PAUSE = pin4, SP_UP = pin5, SP_DOWN = pin6
    - Serial interface (USB to Pi):
        Commands: RESET, SKIP, PAUSE, UP, DOWN, SHUTDOWN, SP:<val>, LOADRECIPE:<t,s;...>
    - Logs over Serial every 1s with timestamp, mode, step name, temps, setpoint, PWM%, button events
*/

#include <PID_v1.h>
#include <math.h>			 

// ----------------------------- PINOUT -----------------------------
const int PIN_SSR = 9;          // PWM output to SSR (via opto SSR recommended)
const int PIN_NTC = A0;         // Vessel averaging NTC
const int PIN_LM35 = A1;        // LM35 (HEX outlet) - mV/°C sensor on analog pin
const int PIN_LED_HEAT = 13;    // Red LED "Heat ON"
const int PIN_LED_OK   = 12;    // Green LED "Mash OK"
const int PIN_ALARM    = 11;    // Alarm/buzzer output (non-blocking blink)
const int PIN_BTN_RESET = 7;
const int PIN_BTN_SKIP  = 8;
const int PIN_BTN_PAUSE = 4;
const int PIN_BTN_SPUP  = 5;
const int PIN_BTN_SPDN  = 6;

// ---------------------- Thermistor / LM35 constants ----------------------
// Steinhart-Hart coefficients from your calibration (use earlier derived)
const double NTCA = 0.001129148;
const double NTCB = 0.000234125;
const double NTCC = 0.0000000876741;
const double SERIES_R = 10000.0; // 10k series resistor for NTC

// LM35: 10 mV / °C, ADC ref 5V -> 1023 steps

// ----------------------------- RECIPE -----------------------------
struct BrewStep {
  const char *name;
  double setpointF;
  unsigned long durationMs; // hold time in ms (0 means "ramp/until reach", or immediate)
  bool heaterOn;           // whether heater should be allowed in this step
  bool triggerOnReach;     // trigger event when reaching setpoint (then move to next step)
  bool triggerOnEnd;       // trigger event when duration completes
};

// Default recipe exactly as described:
//  0: Ramp strike water to 145°F (on reach -> alarm "Add grain" and immediately start step 1 timer)
//  1: Grain rest 10 min (heater OFF, zero flow) (on end -> alarm "Start Pump")
//  2: Mash 148°F, 1 hour (heater ON) (on end -> alarm "Mash Complete")
//  3: Sparge heat 165°F, 30 min (heater ON) (on end -> alarm "Sparge Complete")
//  4: Done (end)
BrewStep defaultRecipe[] = {
  {"Heat to Strike",       145.0,     0UL,       true,  true,  false},
  {"Initial Grain Rest",   145.0,  10UL*60UL*1000UL, false, false, true}, // 10 min
  {"Mash Hold",            148.0,  60UL*60UL*1000UL, true,  false, true},  // 60 min
  {"Sparge Heat",          165.0,  30UL*60UL*1000UL, true,  false, true},  // 30 min
  {"Done",                   0.0,     0UL,       false, false, false}
};
const int DEFAULT_NUM_STEPS = sizeof(defaultRecipe)/sizeof(defaultRecipe[0]);

// We allow up to 12 steps with custom recipes from Pi
const int MAX_STEPS = 12;
BrewStep recipe[MAX_STEPS];
int numSteps = 0;
int currentStep = 0;
unsigned long stepStartMs = 0;
bool recipeTweaked = false;   // true if user adjusted SP or Pi uploaded custom recipe
bool paused = false;
String lastButtonEvent = "";   // transient event label (cleared after logging)

// ----------------------------- PID (vessel control) -----------------------------
// Vessel temp (NTC) is PID input; PID output -> PWM duty 0..255
double vesselTempF = 70.0;
double pidOutput = 0.0;
double pidSetpoint = 145.0;
double Kp=4.0, Ki=0.3, Kd=0.1; // initial / aggressive; update dynamically
PID pid(&vesselTempF, &pidOutput, &pidSetpoint, Kp, Ki, Kd, DIRECT);

// ----------------------------- Failsafe -----------------------------
const double FAIL_MAX_HEX_F = 200.0;   // absolute maximum
const double FAIL_MAX_RATE_F = 5.0;    // °F / second (rate-of-rise)
double lastHexF = 0.0;
unsigned long lastHexTime = 0;
bool failsafeLatched = false;

// ----------------------------- Alarm (non-blocking) -----------------------------
unsigned long alarmUntilMs = 0;
void triggerAlarmNonBlocking(const char *msg, unsigned long durationMs=3000) {
  alarmUntilMs = millis() + durationMs;
  lastButtonEvent = String("ALARM:") + msg; // will be included in next log
}

// ----------------------------- Button debouncing -----------------------------
unsigned long lastButtonDebounceMs = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 200;

// ----------------------------- Logging -----------------------------
unsigned long lastLogMs = 0;
const unsigned long LOG_INTERVAL_MS = 1000;

// ----------------------------- Utility forward prototypes -----------------------------
double readNTC_F();
double readLM35_F();
void loadDefaultRecipe();
void applyRecipeToWorking();
void parseSerialCommand();
void checkButtons();
void advanceStep();
void resetRecipe();
void updatePIDParameters();
void logStatus();
bool checkFailsafe(); // checks LM35 limits & rate, sets failsafeLatched if tripped
void updateAlarmPin();


// ----------------------------- SETUP -----------------------------
void setup() {
  // pins
  pinMode(PIN_SSR, OUTPUT);
  pinMode(PIN_LED_HEAT, OUTPUT);
  pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_ALARM, OUTPUT);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  pinMode(PIN_BTN_SKIP, INPUT_PULLUP);
  pinMode(PIN_BTN_PAUSE, INPUT_PULLUP);
  pinMode(PIN_BTN_SPUP, INPUT_PULLUP);
  pinMode(PIN_BTN_SPDN, INPUT_PULLUP);

  // initialize serial
  Serial.begin(9600);
  while (!Serial) { ; } // wait for USB serial on some boards; harmless on Uno

  // load defaults into working recipe
  loadDefaultRecipe();
  applyRecipeToWorking();

  // init PID
  pidSetpoint = recipe[0].setpointF;
  pid.SetOutputLimits(0, 255);
  pid.SetSampleTime(1000);   // 1 second compute timing
  pid.SetMode(AUTOMATIC);
  updatePIDParameters();

  // start first step
  currentStep = 0;
  stepStartMs = millis();
  recipeTweaked = false;
  paused = false;
  failsafeLatched = false;
  lastButtonEvent = "";
  Serial.println("Controller started: default recipe loaded.");
}

// ----------------------------- MAIN LOOP -----------------------------
void loop() {
  unsigned long now = millis();

  // read sensors
  double vesselTempF = readNTC_F();
  double hexTempF = readLM35_F();

  // update alarm pin non-blocking
  updateAlarmPin();

  // parse serial commands if any (Pi)
  parseSerialCommand();

  // read and handle buttons (debounced)
  checkButtons();

  // check failsafe (LM35-based). latch on trip (manual reset required by RESET)
  if (!failsafeLatched && checkFailsafe()) {
    failsafeLatched = true;
    analogWrite(PIN_SSR, 0); // zero heater
    pidOutput = 0;
    lastButtonEvent = "FAILSAFE_TRIP";
    Serial.println("!!! FAILSAFE TRIPPED: HEX sensor condition !!!");
  }

  // if failsafe latched, freeze everything except logging
  if (failsafeLatched) {
    // leds reflect locked state
    digitalWrite(PIN_LED_HEAT, LOW);
    digitalWrite(PIN_LED_OK, LOW);
    if (now - lastLogMs >= LOG_INTERVAL_MS) { logStatus(); lastLogMs = now; }
    return;
  }

  // Recipe step logic
  BrewStep &step = recipe[currentStep];

  // If step is "ramp to SP" (duration==0) and triggerOnReach==true -> watch for vessel reaching SP
  if (!paused) {
    if (step.durationMs == 0 && step.triggerOnReach) {
      // if heater allowed, PID will run and raise vesselTemp; detect reaching
      // use vesselTempF to detect reaching SP (NTC average). Tolerance 0.5F
      if (fabs(pidSetpoint - vesselTempF) <= 0.5) {
        // reached: trigger event and advance to next step which contains the 10-min rest
        triggerAlarmNonBlocking(step.name);
        advanceStep(); // move to next which typically will be the 10-min rest
      }
    } else {
      // normal time-based step: check time elapsed
      if (step.durationMs > 0 && (now - stepStartMs) >= step.durationMs) {
        // end of step
        if (step.triggerOnEnd) {
          triggerAlarmNonBlocking(step.name);
        }
        advanceStep();
      }
    }
  }

  // PID control (if heater allowed in this step and not paused)
  pidSetpoint = step.setpointF; // PID works to this setpoint but we will respect step.heaterOn
  updatePIDParameters();
  if (!paused && step.heaterOn) {
    pid.Compute();                 // updates pidOutput
    analogWrite(PIN_SSR, (int)pidOutput);
  } else {
    pidOutput = 0;
    analogWrite(PIN_SSR, 0);
  }

  // LED logic
  digitalWrite(PIN_LED_HEAT, (pidOutput > 255 * 0.05) ? HIGH : LOW);
  digitalWrite(PIN_LED_OK, (fabs(vesselTempF - pidSetpoint) <= 0.5) ? HIGH : LOW);

  // logging every 1s
  if (now - lastLogMs >= LOG_INTERVAL_MS) {
    logStatus();
    lastLogMs = now;
    // clear transient button event after logged
    lastButtonEvent = "";
  }

  // small loop delay to avoid busy CPU (PID sample time controls main timing)
  delay(50);
}

// ----------------------------- FUNCTIONS -----------------------------

// Read NTC -> Fahrenheit using Steinhart-Hart
double readNTC_F() {
  int raw = analogRead(PIN_NTC);
  if (raw <= 0) raw = 1;           // avoid divide by zero
  if (raw >= 1023) raw = 1022;
  double v = raw;

  double Vout = v * (5.0 / 1023);
  double r = SERIES_R * (5.0 / Vout - 1.0);
  
  double logR = log(r);
  double tempK = 1.0 / (NTCA + NTCB * logR + NTCC * logR * logR * logR);
  double tempC = tempK - 273.15;
  return (tempC * 9.0 / 5.0) + 32.0;
}

// Read LM35 (mV per °C) -> Fahrenheit
double readLM35_F() {
  int raw = analogRead(PIN_LM35);
  double mv = raw * (5000.0 / 1023.0);
  double degC = mv / 10.0;
  return degC * 9.0 / 5.0 + 32.0;
}

// Check LM35 failsafe: absolute and rate-of-rise
bool checkFailsafe() {
  unsigned long now = millis();
  double hexF = readLM35_F(); // read fresh
  double dt = 0.0;
  if (lastHexTime != 0) {
    dt = (now - lastHexTime) / 1000.0;
    if (dt <= 0) dt = 0.001;
  }
  double rate = (hexF - lastHexF) / dt; // °F / sec
  lastHexF = hexF;
  lastHexTime = now;

  // debug print (optional)
  // Serial.print("HEX:"); Serial.print(hexF); Serial.print(", rate:"); Serial.println(rate);

  if (hexF > FAIL_MAX_HEX_F || rate > FAIL_MAX_RATE_F) {
    return true;
  }
  return false;
}

// Load default recipe into recipe[] working array
void loadDefaultRecipe() {
  numSteps = DEFAULT_NUM_STEPS;
  for (int i=0; i<DEFAULT_NUM_STEPS && i<MAX_STEPS; ++i) {
    recipe[i] = defaultRecipe[i];
  }
}

// Copy default recipe into working recipe (useful for RESET)
void applyRecipeToWorking() {
  loadDefaultRecipe();
}

// Advance to next step (used for SKIP and automatic transitions)
void advanceStep() {
  if (currentStep < numSteps - 1) {
    currentStep++;
    stepStartMs = millis();
    recipeTweaked = recipeTweaked; // keep flag meaning: still tweaked if set earlier
    // update pidSetpoint to new step's setpoint
    pidSetpoint = recipe[currentStep].setpointF;
    lastButtonEvent = String("STEP_ADVANCE:") + recipe[currentStep].name;
    Serial.print("ADVANCE -> Step "); Serial.print(currentStep);
    Serial.print(" : "); Serial.println(recipe[currentStep].name);
  } else {
    // already at final step
    Serial.println("Recipe complete (end).");
    // disable heating
    analogWrite(PIN_SSR, 0);
    pidOutput = 0;
  }
}

// Reset recipe to default and restart at step 0
void resetRecipe() {
  applyRecipeToWorking();
  numSteps = DEFAULT_NUM_STEPS;
  currentStep = 0;
  stepStartMs = millis();
  paused = false;
  recipeTweaked = false;
  failsafeLatched = false;   // allow reset to clear failsafe latch
  lastButtonEvent = "RESET";
  Serial.println("Recipe reset to default.");
}

// Parse serial commands from Pi
// Supported: RESET, SKIP, PAUSE, UP, DOWN, SHUTDOWN, SP:<value>, LOADRECIPE:<sp,sec;...>
void parseSerialCommand() {
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    // uppercase comparison
    String cmd = line;
    cmd.toUpperCase();

    if (cmd == "RESET") { resetRecipe(); }
    else if (cmd == "SKIP") { advanceStep(); lastButtonEvent="SKIP"; }
    else if (cmd == "PAUSE") { paused = !paused; lastButtonEvent = paused ? "PAUSE" : "RESUME"; }
    else if (cmd == "UP") { recipe[currentStep].setpointF += 1.0; pidSetpoint = recipe[currentStep].setpointF; recipeTweaked = true; lastButtonEvent="UP"; }
    else if (cmd == "DOWN") { recipe[currentStep].setpointF -= 1.0; pidSetpoint = recipe[currentStep].setpointF; recipeTweaked = true; lastButtonEvent="DOWN"; }
    else if (cmd == "SHUTDOWN") { analogWrite(PIN_SSR,0); pidOutput = 0; paused = true; lastButtonEvent="SHUTDOWN"; }
    else if (cmd.startsWith("SP:")) {
      double v = cmd.substring(3).toFloat();
      recipe[currentStep].setpointF = v;
      pidSetpoint = v;
      recipeTweaked = true;
      lastButtonEvent = String("SP_SET:") + String(v,1);
      Serial.print("Serial: Setpoint set to "); Serial.println(v);
    }
    else if (cmd.startsWith("LOADRECIPE:")) {
      // Format: LOADRECIPE:temp,sec;temp,sec;...
      String payload = line.substring(11); // keep original case for parsing numbers
      // parse segments separated by ';'
      int idx = 0;
      int stepIdx = 0;
      while (payload.length() > 0 && stepIdx < MAX_STEPS) {
        int semi = payload.indexOf(';');
        String seg;
        if (semi == -1) { seg = payload; payload = ""; }
        else { seg = payload.substring(0, semi); payload = payload.substring(semi+1); }
        seg.trim();
        if (seg.length() == 0) continue;
        int comma = seg.indexOf(',');
        if (comma == -1) continue;
        double t = seg.substring(0, comma).toFloat();
        unsigned long secs = (unsigned long)seg.substring(comma+1).toInt();
        recipe[stepIdx].name = NULL; // custom no-name (we'll label later)
        recipe[stepIdx].setpointF = t;
        recipe[stepIdx].durationMs = secs * 1000UL;
        recipe[stepIdx].heaterOn = (t > 0.1); // allow heater when SP > 0
        recipe[stepIdx].triggerOnReach = (recipe[stepIdx].durationMs==0); // simple rule
        recipe[stepIdx].triggerOnEnd = (recipe[stepIdx].durationMs>0);
        stepIdx++;
      }
      if (stepIdx>0) {
        numSteps = stepIdx;
        currentStep = 0;
        pidSetpoint = recipe[0].setpointF;
        stepStartMs = millis();
        recipeTweaked = true;
        lastButtonEvent = "LOADRECIPE";
        Serial.print("Loaded custom recipe with "); Serial.print(numSteps); Serial.println(" steps.");
      }
    }
    else {
      Serial.print("Unknown cmd: "); Serial.println(line);
    }
  }
}

// Read and debounce buttons; treat active LOW (INPUT_PULLUP)
void checkButtons() {
  unsigned long now = millis();
  if (now - lastButtonDebounceMs < BUTTON_DEBOUNCE_MS) return;

  if (digitalRead(PIN_BTN_RESET) == LOW) {
    resetRecipe();
    lastButtonDebounceMs = now;
    lastButtonEvent = "BTN_RESET";
  } else if (digitalRead(PIN_BTN_SKIP) == LOW) {
    advanceStep();
    lastButtonDebounceMs = now;
    lastButtonEvent = "BTN_SKIP";
  } else if (digitalRead(PIN_BTN_PAUSE) == LOW) {
    paused = !paused;
    lastButtonDebounceMs = now;
    lastButtonEvent = paused ? "BTN_PAUSE" : "BTN_RESUME";
  } else if (digitalRead(PIN_BTN_SPUP) == LOW) {
    recipe[currentStep].setpointF += 1.0;
    pidSetpoint = recipe[currentStep].setpointF;
    recipeTweaked = true;
    lastButtonDebounceMs = now;
    lastButtonEvent = "BTN_UP";
  } else if (digitalRead(PIN_BTN_SPDN) == LOW) {
    recipe[currentStep].setpointF -= 1.0;
    pidSetpoint = recipe[currentStep].setpointF;
    recipeTweaked = true;
    lastButtonDebounceMs = now;
    lastButtonEvent = "BTN_DOWN";
  }
}

// Dynamic PID tuning based on distance from setpoint (ΔT)
void updatePIDParameters() {
  double delta = fabs(pidSetpoint - vesselTempF);
  // aggressive when far, moderate when mid, stable when near
  if (delta > 10.0) {
    Kp = 4.0; Ki = 0.30; Kd = 0.10;   // fast heat-up
  } else if (delta > 2.0) {
    Kp = 2.0; Ki = 0.20; Kd = 0.30;   // settling
  } else {
    Kp = 1.0; Ki = 0.05; Kd = 0.50;   // stable hold
  }
  pid.SetTunings(Kp, Ki, Kd);
}

// Non-blocking alarm pin updater (blink while alarmUntilMs not reached)
void updateAlarmPin() {
  if (millis() < alarmUntilMs) digitalWrite(PIN_ALARM, HIGH);
  else digitalWrite(PIN_ALARM, LOW);
}

// Log current status to Serial in compact CSV-like line
void logStatus() {
  unsigned long t = millis();
  // Step name (if null for custom steps, show index)
  String stepName;
  if (recipe[currentStep].name != NULL) stepName = String(recipe[currentStep].name);
  else stepName = String("Step") + String(currentStep);

  // PWM percent
  int pwmPct = (int)round((pidOutput / 255.0) * 100.0);

  // Mode: Default or Tweaked or Paused or Failsafe
  String mode = "RUNNING";
  if (paused) mode = "PAUSED";
  else if (failsafeLatched) mode = "FAILSAFE";
  else if (recipeTweaked) mode = "TWEAKED";

  // Compose log
  Serial.print("TIME="); Serial.print(t);
  Serial.print(",MODE="); Serial.print(mode);
  Serial.print(",STEP="); Serial.print(currentStep);
  Serial.print(",STEPNAME="); Serial.print(stepName);
  Serial.print(",SP="); Serial.print(pidSetpoint,1);
  Serial.print(",NTC="); Serial.print(vesselTempF,2);
  Serial.print(",HEX="); Serial.print(readLM35_F(),2); // fresh read
  Serial.print(",PWM="); Serial.print(pwmPct);
  if (lastButtonEvent.length()>0) {
    Serial.print(",EVENT="); Serial.print(lastButtonEvent);
  }
  Serial.println();
}

// ----------------------------- end of sketch -----------------------------
