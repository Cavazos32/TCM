#include <Arduino.h>
 
HardwareSerial lidar(2);
 
#define LIDAR_RX 16
#define LIDAR_TX 17
 
// Intervalo inicial: 1 segundo
unsigned long printInterval = 1000;
unsigned long lastPrint = 0;
 
// Ultima lectura valida
uint16_t distance = 0;
uint16_t strength = 0;
bool validReading = false;
 
// Recepcion de trama
uint8_t frame[9];
uint8_t frameIndex = 0;
 
void setup() {
  Serial.begin(115200);
 
  lidar.begin(115200, SERIAL_8N1,
              LIDAR_RX, LIDAR_TX);
 
  Serial.println("TFmini Plus iniciado");
  Serial.println("-------------------------");
  Serial.println("Ingrese intervalo en segundos:");
  Serial.println("Ejemplo: 1, 5, 10, 0.5");
  Serial.println("-------------------------");
}
 
void loop() {
 
  // 1. Leer intervalo desde monitor serial
  if (Serial.available() > 0) {
 
    String input = Serial.readStringUntil('\n');
    input.trim();
 
    if (input.length() > 0) {
      float seconds = input.toFloat();
 
      if (seconds > 0 && seconds <= 3600) {
 
        printInterval =
          (unsigned long)(seconds * 1000.0f);
 
        lastPrint = millis();
 
        Serial.print("Nuevo intervalo: ");
        Serial.print(seconds);
        Serial.println(" segundos");
 
      } else {
        Serial.println("Ingrese un valor entre 0 y 3600 segundos.");
      }
    }
  }
 
  // 2. Leer continuamente el TFmini Plus
  while (lidar.available() > 0) {
 
    uint8_t incomingByte = lidar.read();
 
    // Buscar primer byte de cabecera
    if (frameIndex == 0) {
      if (incomingByte == 0x59) {
        frame[frameIndex++] = incomingByte;
      }
      continue;
    }
 
    // Verificar segundo byte de cabecera
    if (frameIndex == 1) {
      if (incomingByte == 0x59) {
        frame[frameIndex++] = incomingByte;
      } else {
        frameIndex = 0;
      }
      continue;
    }
 
    frame[frameIndex++] = incomingByte;
 
    if (frameIndex == 9) {
 
      frameIndex = 0;
 
      uint16_t checksum = 0;
 
      for (int i = 0; i < 8; i++) {
        checksum += frame[i];
      }
 
      if ((checksum & 0xFF) == frame[8]) {
 
        distance =
          frame[2] | (frame[3] << 8);
 
        strength =
          frame[4] | (frame[5] << 8);
 
        validReading = true;
      }
    }
  }
 
  // 3. Imprimir segun intervalo seleccionado
  if (millis() - lastPrint >= printInterval) {
 
    lastPrint = millis();
 
    if (validReading) {
 
      Serial.print("Distancia: ");
      Serial.print(distance);
      Serial.print(" cm | Señal: ");
      Serial.println(strength);
 
    } else {
 
      Serial.println("Esperando lectura valida...");
    }
  }
}