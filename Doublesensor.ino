// Define sensor pins using arrays
const int trigPins[2] = {9, 11};   // Trigger pins for sensors
const int echoPins[2] = {10, 12};  // Echo pins for sensors
// Array to store distances
int distances[2];
void setup() {
  // Initialize all sensor pins
  for (int i = 0; i < 2; i++) {
    pinMode(trigPins[i], OUTPUT);
    pinMode(echoPins[i], INPUT);
  }
  Serial.begin(9600);
  Serial.println("Dual HC-SR04 Sensor Test - Array Version");
  Serial.println("=========================================");
}
void loop() {
  // Read all sensors
  for (int i = 0; i < 2; i++) {
    distances[i] = readDistance(trigPins[i], echoPins[i]);
    delay(10); // Small delay between readings
  }
  // Display all distances
  for (int i = 0; i < 2; i++) {
    Serial.print("Sensor ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(distances[i]);
    Serial.print(" cm");
    if (i < 1) Serial.print(" | "); // Add separator between sensors
  }
  Serial.println(); // New line after all sensors
  delay(500); // Wait half second before next reading
}
// Function to read distance from any sensor
int readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH);
  int distance = duration * 0.034 / 2;
  return distance;
}




