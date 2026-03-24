// ------------------------------------------------------------
// TB6612FNG + HC-SR04 + HC-05 BLUETOOTH TRIP CONTROL
// Arduino Uno
//
// Bluetooth commands come from HC-05 on SoftwareSerial.
// USB Serial is kept for debugging / Serial Monitor.
//
// HC-05 wiring (recommended):
//   HC-05 TXD -> Arduino D2   (BT_RX_PIN below)
//   HC-05 RXD -> Arduino D3   (BT_TX_PIN below, use voltage divider)
//   HC-05 VCC -> 5V
//   HC-05 GND -> GND
//
// Commands from laptop:
//   TRIP <out_dir> <pwm> <out_ms> <pause_ms> <back_dir> <back_ms>
//   STOP
// ------------------------------------------------------------

#include <SoftwareSerial.h>

// ---- HC-05 Bluetooth serial ----
const int BT_RX_PIN = 2;  // Arduino receives from HC-05 TXD
const int BT_TX_PIN = 3;  // Arduino sends to HC-05 RXD
SoftwareSerial btSerial(BT_RX_PIN, BT_TX_PIN);

// ---- Motor A pins ----
const int PWMA = 10;
const int AIN1 = 8;
const int AIN2 = 9;

// ---- Motor B pins ----
const int PWMB = 5;
const int BIN1 = 6;
const int BIN2 = 7;

// ---- Standby pin ----
const int STBY = 4;

// ---- Ultrasonic Sensor ----
const int TRIG = 11;
const int ECHO = 12;

// ---- Safety thresholds ----
const long STOP_CM = 8;
const long RESUME_CM = STOP_CM + 3;
const unsigned long SENSOR_PERIOD_MS = 50;

// ---- Trip phases ----
enum Phase {
  IDLE,
  MOVING_OUT,
  PAUSE_AT_ROW,
  RETURNING_HOME
};

Phase phase = IDLE;

bool moving = false;
bool paused_for_obstacle = false;
bool paused_for_dwell = false;

unsigned long move_end_ms = 0;
unsigned long remaining_ms = 0;
unsigned long dwell_end_ms = 0;

char current_dir = 'F';
int current_pwm = 0;

char out_dir = 'F';
char back_dir = 'B';
int trip_pwm = 0;
unsigned long out_ms = 0;
unsigned long back_ms = 0;
unsigned long pause_ms = 10000;

void logLine(const String &msg) {
  Serial.println(msg);
  btSerial.println(msg);
}

void setMotorA(int speed) {
  speed = constrain(speed, -255, 255);
  if (speed >= 0) {
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
    analogWrite(PWMA, speed);
  } else {
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, HIGH);
    analogWrite(PWMA, -speed);
  }
}

void setMotorB(int speed) {
  speed = constrain(speed, -255, 255);
  if (speed >= 0) {
    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, LOW);
    analogWrite(PWMB, speed);
  } else {
    digitalWrite(BIN1, LOW);
    digitalWrite(BIN2, HIGH);
    analogWrite(PWMB, -speed);
  }
}

void stopMotors() {
  analogWrite(PWMA, 0);
  analogWrite(PWMB, 0);
  moving = false;
}

void drive(char dir, int pwm) {
  pwm = constrain(pwm, 0, 255);
  if (dir == 'F') {
    setMotorA(pwm);
    setMotorB(pwm);
  } else if (dir == 'B') {
    setMotorA(-pwm);
    setMotorB(-pwm);
  }
}

long getDistanceCm() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duration = pulseIn(ECHO, HIGH, 30000);
  if (duration == 0) return 9999;
  return (long)(duration * 0.034 / 2.0);
}

void beginMove(char dir, int pwm, unsigned long durationMs) {
  current_dir = dir;
  current_pwm = constrain(pwm, 0, 255);
  remaining_ms = durationMs;

  drive(current_dir, current_pwm);
  moving = true;
  paused_for_obstacle = false;
  move_end_ms = millis() + remaining_ms;

  logLine("MOVE_START " + String(current_dir) + " " + String(current_pwm) + " " + String(durationMs));
}

void beginDwell(unsigned long durationMs) {
  stopMotors();
  paused_for_dwell = true;
  dwell_end_ms = millis() + durationMs;
  logLine("AT_ROW_WAIT " + String(durationMs));
}

void finishTrip() {
  stopMotors();
  phase = IDLE;
  paused_for_obstacle = false;
  paused_for_dwell = false;
  remaining_ms = 0;
  logLine("DONE");
}

void cancelTrip() {
  stopMotors();
  phase = IDLE;
  paused_for_obstacle = false;
  paused_for_dwell = false;
  remaining_ms = 0;
  logLine("STOPPED");
}

void pauseForObstacle(long distanceCm) {
  if (!moving) return;

  unsigned long now = millis();
  if ((long)(move_end_ms - now) > 0) {
    remaining_ms = move_end_ms - now;
  } else {
    remaining_ms = 0;
  }

  stopMotors();
  paused_for_obstacle = true;
  logLine("SAFETY_STOP " + String(distanceCm) + " REMAINING_MS " + String(remaining_ms));
}

void resumeAfterObstacleClears() {
  if (!paused_for_obstacle) return;

  if (remaining_ms == 0) {
    paused_for_obstacle = false;
    if (phase == MOVING_OUT) {
      phase = PAUSE_AT_ROW;
      beginDwell(pause_ms);
    } else if (phase == RETURNING_HOME) {
      finishTrip();
    }
    return;
  }

  drive(current_dir, current_pwm);
  moving = true;
  paused_for_obstacle = false;
  move_end_ms = millis() + remaining_ms;

  logLine("RESUMED " + String(current_dir) + " " + String(current_pwm) + " " + String(remaining_ms));
}

bool parseTripCommand(String cmd) {
  char outDir;
  int pwm;
  unsigned long outDur;
  unsigned long pauseDur;
  char backDir;
  unsigned long backDur;

  int matched = sscanf(
    cmd.c_str(),
    "TRIP %c %d %lu %lu %c %lu",
    &outDir, &pwm, &outDur, &pauseDur, &backDir, &backDur
  );

  if (matched != 6) {
    logLine("ERR BAD_FORMAT");
    return false;
  }

  outDir = toupper(outDir);
  backDir = toupper(backDir);

  if (!((outDir == 'F') || (outDir == 'B'))) {
    logLine("ERR BAD_OUT_DIR");
    return false;
  }
  if (!((backDir == 'F') || (backDir == 'B'))) {
    logLine("ERR BAD_BACK_DIR");
    return false;
  }

  pwm = constrain(pwm, 0, 255);

  out_dir = outDir;
  back_dir = backDir;
  trip_pwm = pwm;
  out_ms = outDur;
  back_ms = backDur;
  pause_ms = pauseDur;
  return true;
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "STOP") {
    cancelTrip();
    return;
  }

  if (cmd.startsWith("TRIP")) {
    if (!parseTripCommand(cmd)) return;

    phase = MOVING_OUT;
    paused_for_dwell = false;
    paused_for_obstacle = false;

    logLine(
      "OK TRIP " + String(out_dir) + " " + String(trip_pwm) + " " +
      String(out_ms) + " " + String(pause_ms) + " " +
      String(back_dir) + " " + String(back_ms)
    );

    if (out_ms == 0) {
      phase = PAUSE_AT_ROW;
      beginDwell(pause_ms);
    } else {
      beginMove(out_dir, trip_pwm, out_ms);
    }
    return;
  }

  logLine("ERR UNKNOWN_CMD");
}

void setup() {
  Serial.begin(9600);
  btSerial.begin(9600);

  pinMode(PWMA, OUTPUT);
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(PWMB, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
  pinMode(STBY, OUTPUT);
  digitalWrite(STBY, HIGH);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  stopMotors();
  logLine("READY");
}

void loop() {
  if (btSerial.available()) {
    String cmd = btSerial.readStringUntil('\n');
    handleCommand(cmd);
  }

  // Optional USB fallback for bench testing while tethered.
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleCommand(cmd);
  }

  unsigned long now = millis();
  static unsigned long lastSense = 0;
  if (now - lastSense >= SENSOR_PERIOD_MS) {
    lastSense = now;
    long d = getDistanceCm();

    if (moving && d <= STOP_CM) {
      pauseForObstacle(d);
    } else if (paused_for_obstacle && d > RESUME_CM) {
      resumeAfterObstacleClears();
    }
  }

  if (moving && (long)(millis() - move_end_ms) >= 0) {
    stopMotors();
    paused_for_obstacle = false;
    remaining_ms = 0;

    if (phase == MOVING_OUT) {
      phase = PAUSE_AT_ROW;
      beginDwell(pause_ms);
    } else if (phase == RETURNING_HOME) {
      finishTrip();
    }
  }

  if (phase == PAUSE_AT_ROW && paused_for_dwell && (long)(millis() - dwell_end_ms) >= 0) {
    paused_for_dwell = false;

    if (back_ms == 0) {
      finishTrip();
    } else {
      phase = RETURNING_HOME;
      beginMove(back_dir, trip_pwm, back_ms);
    }
  }
}
