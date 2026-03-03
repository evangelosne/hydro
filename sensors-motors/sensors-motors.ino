// -------------------------------
// TB6612FNG + HC-SR04 + SERIAL MOVE CONTROL
// Arduino Uno
// Commands from laptop:
//   MOVE F <pwm0-255> <duration_ms>
//   MOVE B <pwm0-255> <duration_ms>
//   STOP
// -------------------------------

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

// ---- Safety threshold ----
const long STOP_CM = 8;        // stop if obstacle closer than this
const unsigned long SENSOR_PERIOD_MS = 50;

// Internal state
String line;
bool moving = false;
unsigned long move_end_ms = 0;

// -------------------------------
// Motor helpers (NOW supports forward/back)
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

// -------------------------------
// Read distance in cm
long getDistanceCm() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duration = pulseIn(ECHO, HIGH, 30000); // 30ms timeout
  if (duration == 0) return 9999;             // no echo => treat as far away
  return (long)(duration * 0.034 / 2.0);
}

// -------------------------------
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

// -------------------------------
void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "STOP") {
    stopMotors();
    Serial.println("STOPPED");
    return;
  }

  // Expected: MOVE F 170 3000
  if (cmd.startsWith("MOVE")) {
    int s1 = cmd.indexOf(' ');
    int s2 = cmd.indexOf(' ', s1 + 1);
    int s3 = cmd.indexOf(' ', s2 + 1);

    if (s1 < 0 || s2 < 0 || s3 < 0) {
      Serial.println("ERR BAD_FORMAT");
      return;
    }

    char dir = toupper(cmd.charAt(s1 + 1));
    int pwm = cmd.substring(s2 + 1, s3).toInt();
    long dur = cmd.substring(s3 + 1).toInt();

    if (!(dir == 'F' || dir == 'B')) {
      Serial.println("ERR BAD_DIR");
      return;
    }
    pwm = constrain(pwm, 0, 255);
    if (dur < 0) dur = 0;

    // start move
    drive(dir, pwm);
    moving = true;
    move_end_ms = millis() + (unsigned long)dur;

    Serial.print("OK MOVE ");
    Serial.print(dir);
    Serial.print(" ");
    Serial.print(pwm);
    Serial.print(" ");
    Serial.println(dur);
    return;
  }

  Serial.println("ERR UNKNOWN_CMD");
}

// -------------------------------
void loop() {
  // 1) read serial commands (non-blocking)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleCommand(cmd);
  }

  // 2) safety obstacle check + motion completion
  static unsigned long lastSense = 0;
  unsigned long now = millis();

  if (now - lastSense >= SENSOR_PERIOD_MS) {
    lastSense = now;

    long d = getDistanceCm();
    if (moving && d <= STOP_CM) {
      stopMotors();
      Serial.print("SAFETY_STOP ");
      Serial.println(d);
    }
  }

  // 3) stop after duration
  if (moving && (long)(millis() - move_end_ms) >= 0) {
    stopMotors();
    Serial.println("DONE");
  }
}