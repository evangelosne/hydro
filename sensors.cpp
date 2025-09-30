#define TRIG_PIN 9
#define ECHO_PIN 10

void setup() {
  Serial.begin(9600); // start serial monitor
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
}

void loop() {
  // Clear trigger pin
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  // Send 10µs HIGH pulse to trigger
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Read echo pulse duration
  long duration = pulseIn(ECHO_PIN, HIGH);

  // Convert duration to distance (in cm)
  // speed of sound = 343 m/s = 0.0343 cm/µs
  int distance = duration * 0.0343 / 2;

  // Print distance
  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  delay(500); // wait before next reading
}
