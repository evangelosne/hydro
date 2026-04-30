// ------------------------------------------------------------
// TB6612FNG + HC-SR04 + SERIAL TRIP CONTROL
// Arduino Uno
//
// Commands from laptop:
//   TRIP <out_dir> <pwm> <out_ms> <pause_ms> <back_dir> <back_ms>
//   STOP
//
// Example:
//   TRIP F 245 3000 10000 B 3000
//
// Behavior:
//   1. Move outward
//   2. Stop at destination for pause_ms
//   3. Return home
//   4. Print DONE when fully complete
//
// Obstacle behavior:
//   - If obstacle is too close, pause
//   - Resume automatically when obstacle clears
// ------------------------------------------------------------

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
const long STOP_CM = 40;
const long RESUME_CM = STOP_CM + 5;
const unsigned long SENSOR_PERIOD_MS = 50;

// ---- Trip phases ----
enum Phase {
  IDLE,
  MOVING_OUT,
  PAUSE_AT_ROW,
  RETURNING_HOME
};

Phase phase = IDLE;

// Motion state
bool moving = false;
bool paused_for_obstacle = false;
bool paused_for_dwell = false;

unsigned long move_end_ms = 0;
unsigned long remaining_ms = 0;
unsigned long dwell_end_ms = 0;

char current_dir = 'F';
int current_pwm = 0;

// Planned trip details
char out_dir = 'F';
char back_dir = 'B';
int trip_pwm = 0;
unsigned long out_ms = 0;
unsigned long back_ms = 0;
unsigned long pause_ms = 10000;

// ------------------------------------------------------------
// Motor helpers
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

// ------------------------------------------------------------
// Distance sensor
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

// ------------------------------------------------------------
// Phase control
void beginMove(char dir, int pwm, unsigned long durationMs) {
  current_dir = dir;
  current_pwm = constrain(pwm, 0, 255);
  remaining_ms = durationMs;

  drive(current_dir, current_pwm);
  moving = true;
  paused_for_obstacle = false;
  move_end_ms = millis() + remaining_ms;

  Serial.print("MOVE_START ");
  Serial.print(current_dir);
  Serial.print(" ");
  Serial.print(current_pwm);
  Serial.print(" ");
  Serial.println(durationMs);
}

void beginDwell(unsigned long durationMs) {
  stopMotors();
  paused_for_dwell = true;
  dwell_end_ms = millis() + durationMs;

  Serial.print("AT_ROW_WAIT ");
  Serial.println(durationMs);
}

void finishTrip() {
  stopMotors();
  phase = IDLE;
  paused_for_obstacle = false;
  paused_for_dwell = false;
  remaining_ms = 0;
  Serial.println("DONE");
}

void cancelTrip() {
  stopMotors();
  phase = IDLE;
  paused_for_obstacle = false;
  paused_for_dwell = false;
  remaining_ms = 0;
  Serial.println("STOPPED");
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

  Serial.print("SAFETY_STOP ");
  Serial.print(distanceCm);
  Serial.print(" REMAINING_MS ");
  Serial.println(remaining_ms);
}

void resumeAfterObstacleClears() {
  if (!paused_for_obstacle) return;

  if (remaining_ms == 0) {
    // Treat as phase completed
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

  Serial.print("RESUMED ");
  Serial.print(current_dir);
  Serial.print(" ");
  Serial.print(current_pwm);
  Serial.print(" ");
  Serial.println(remaining_ms);
}

// ------------------------------------------------------------
// Parsing helpers
bool parseTripCommand(String cmd) {
  // Expected:
  // TRIP <out_dir> <pwm> <out_ms> <pause_ms> <back_dir> <back_ms>

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
    Serial.println("ERR BAD_FORMAT");
    return false;
  }

  outDir = toupper(outDir);
  backDir = toupper(backDir);

  if (!((outDir == 'F') || (outDir == 'B'))) {
    Serial.println("ERR BAD_OUT_DIR");
    return false;
  }

  if (!((backDir == 'F') || (backDir == 'B'))) {
    Serial.println("ERR BAD_BACK_DIR");
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

// ------------------------------------------------------------
void setup() {
  Serial.begin(9600);

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
  Serial.println("READY");
}

// ------------------------------------------------------------
void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "STOP") {
    cancelTrip();
    return;
  }

  if (cmd.startsWith("TRIP")) {
    if (!parseTripCommand(cmd)) {
      return;
    }

    phase = MOVING_OUT;
    paused_for_dwell = false;
    paused_for_obstacle = false;

    Serial.print("OK TRIP ");
    Serial.print(out_dir);
    Serial.print(" ");
    Serial.print(trip_pwm);
    Serial.print(" ");
    Serial.print(out_ms);
    Serial.print(" ");
    Serial.print(pause_ms);
    Serial.print(" ");
    Serial.print(back_dir);
    Serial.print(" ");
    Serial.println(back_ms);

    if (out_ms == 0) {
      phase = PAUSE_AT_ROW;
      beginDwell(pause_ms);
    } else {
      beginMove(out_dir, trip_pwm, out_ms);
    }

    return;
  }

  Serial.println("ERR UNKNOWN_CMD");
}

// ------------------------------------------------------------
void loop() {
  // 1) Read serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleCommand(cmd);
  }

  unsigned long now = millis();

  // 2) Obstacle pause/resume only while actively moving
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

  // 3) Handle move completion
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

  // 4) Handle dwell completion
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