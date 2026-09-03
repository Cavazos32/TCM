// Andon — salidas visuales/sonoras (rojo, amarillo, verde, buzzer).
// WiFi STA 10.10.32.60 | TCP :8768 (pendiente).
// Esqueleto: solo declaración de pines; sin lógica de red ni protocolo.

#include "Config.h"

void setup()
{
  pinMode(PIN_RED_LED, OUTPUT);
  pinMode(PIN_YELLOW_LED, OUTPUT);
  pinMode(PIN_GREEN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

#if ANDON_ACTIVE_HIGH
  digitalWrite(PIN_RED_LED, LOW);
  digitalWrite(PIN_YELLOW_LED, LOW);
  digitalWrite(PIN_GREEN_LED, LOW);
  digitalWrite(PIN_BUZZER, LOW);
#else
  digitalWrite(PIN_RED_LED, HIGH);
  digitalWrite(PIN_YELLOW_LED, HIGH);
  digitalWrite(PIN_GREEN_LED, HIGH);
  digitalWrite(PIN_BUZZER, HIGH);
#endif
}

void loop()
{
}
