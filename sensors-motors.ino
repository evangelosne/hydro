// -------------------------------
// TB6612FNG + HC-SR04 obstacle logic
// Arduino Uno
// -------------------------------

// ---- Motor A pins ----
const int PWMA = 10;      // PWM speed control A
const int AIN1 = 8;       // Direction A
const int AIN2 = 9;       

// ---- Motor B pins ----
const int PWMB = 5;       // PWM speed control B
const int BIN1 = 6;       // Direction B
const int BIN2 = 7;

// ---- Standby pin ----
const int STBY = 4;       // Must be HIGH

// ---- Ultrasonic Sensor ----
const int TRIG = 11;
const int ECHO = 12;

// ---- Speed limits ----
int maxSpeed = 255;        // Full speed
int slowSpeed = 120;       // Slow-down speed
int currentSpeed = 0;      // For smooth acceleration

// -------------------------------
void setup() {
  Serial.begin(9600);

  // Motor pins
  pinMode(PWMA, OUTPUT);
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);

  pinMode(PWMB, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);

  pinMode(STBY, OUTPUT);
  digitalWrite(STBY, HIGH);

  // HC-SR04 pins
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  Serial.println("System started...");
}

// -------------------------------
// Motor control helpers
void motorA(int speed) {
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);
  analogWrite(PWMA, speed);
}

void motorB(int speed) {
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMB, speed);
}

void setMotors(int speed) {
  motorA(speed);
  motorB(speed);
}

// -------------------------------
// Read distance in cm
long getDistance() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duration = pulseIn(ECHO, HIGH, 30000); // 30ms timeout
  long distance = duration * 0.034 / 2;

  return distance;
}

// -------------------------------
// MAIN LOOP
void loop() {

  long distance = getDistance();

  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.print(" cm   |   Speed: ");
  Serial.println(currentSpeed);

  // -----------------------------
  // OBSTACLE LOGIC
  // -----------------------------

  if (distance < 3) {
    // TOO CLOSE → full stop
    currentSpeed = 0;
    setMotors(0);
    Serial.println("STOP - object < 3 cm");
  }

  else if (distance >= 3 && distance <= 6) {
    // SLOW DOWN zone
    if (currentSpeed > slowSpeed)
      currentSpeed -= 5;         // Smooth deceleration
    else
      currentSpeed = slowSpeed;

    setMotors(currentSpeed);
    Serial.println("SLOW - object 3-6 cm");
  }

  else if (distance > 6) {
    // CLEAR PATH → accelerate gradually
    if (currentSpeed < maxSpeed)
      currentSpeed += 5;         // Smooth acceleration

    if (currentSpeed > maxSpeed)
      currentSpeed = maxSpeed;

    setMotors(currentSpeed);
    Serial.println("CLEAR - accelerating");
  }

  delay(50);   // 20 checks per second
}
