// Andon — torreta + presión FRL.
// HMI manda ANDON_RX_* (0x40–0x49). Presión: pin local → torreta Error + TX 0x50 (sin RX).

#include "Config.h"
#include "AndonStates.h"

static uint8_t lastMachineByte = ANDON_RX_IDLE;
static bool pressureFaultLatched = false;

static void andonWriteOut(uint8_t pin, bool on)
{
#if ANDON_ACTIVE_HIGH
  digitalWrite(pin, on ? HIGH : LOW);
#else
  digitalWrite(pin, on ? LOW : HIGH);
#endif
}

void andonSetGreen(bool on)  { andonWriteOut(PIN_GREEN_LED, on); }
void andonSetYellow(bool on) { andonWriteOut(PIN_YELLOW_LED, on); }
void andonSetRed(bool on)    { andonWriteOut(PIN_RED_LED, on); }
void andonSetBuzzer(bool on) { andonWriteOut(PIN_BUZZER, on); }

void andonTowerAllOff()
{
  andonSetGreen(false);
  andonSetYellow(false);
  andonSetRed(false);
  andonSetBuzzer(false);
}

void andonApplyMachineByte(uint8_t byteCode)
{
  if (!andonIsMachineByte(byteCode)) return;
  // Presión local manda sobre HMI mientras el FRL esté en falla
  if (pressureFaultLatched) return;

  lastMachineByte = byteCode;
  andonTowerAllOff();

  switch (byteCode) {
    case ANDON_RX_INIT:
    case ANDON_RX_IDLE:
    case ANDON_RX_BUSY:
    case ANDON_RX_START:
    case ANDON_RX_RESET:
    case ANDON_RX_RETURN:
      andonSetGreen(true);
      break;
    case ANDON_RX_STOP:
      andonSetRed(true);
      break;
    case ANDON_RX_ERROR:
      andonSetRed(true);
      andonSetBuzzer(true);
      break;
    case ANDON_RX_FINISH:
      andonSetGreen(true);
      andonSetBuzzer(true);
      break;
    case ANDON_RX_MATERIALIST:
      andonSetYellow(true);
      andonSetBuzzer(true);
      break;
    default:
      break;
  }
}

void PressureError()
{
  // TODO: TCP :8768 → HMI {"byte":0x50,...} ANDON_ERR_PRESSURE
#if ANDON_DEBUG
  Serial.printf("Andon TX PressureError 0x%02X\n", (unsigned)ANDON_ERR_PRESSURE);
#endif
}

bool andonPressureFaultRaw()
{
  return digitalRead(PIN_PRESSURE_FRL) == LOW;
}

void andonServicePressure()
{
  static bool raw = false, stable = false;
  static uint32_t tChange = 0;
  const uint32_t now = millis();
  const bool r = andonPressureFaultRaw();

  if (r != raw) {
    raw = r;
    tChange = now;
  } else if ((now - tChange) >= ANDON_PRESSURE_DEBOUNCE_MS && r != stable) {
    stable = r;
    if (stable) {
      pressureFaultLatched = true;
      andonTowerAllOff();
      andonSetRed(true);
      andonSetBuzzer(true);
      PressureError();  // avisar HMI (0x50); torreta ya activada aquí
    } else {
      pressureFaultLatched = false;
      andonApplyMachineByte(lastMachineByte);  // volver al último estado HMI
    }
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(PIN_RED_LED, OUTPUT);
  pinMode(PIN_YELLOW_LED, OUTPUT);
  pinMode(PIN_GREEN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_PRESSURE_FRL, INPUT_PULLUP);

  pinMode(PIN_ETH_CS, OUTPUT);
  pinMode(PIN_ETH_CLK, OUTPUT);
  pinMode(PIN_ETH_RST, OUTPUT);
  pinMode(PIN_ETH_MOSI, OUTPUT);
  pinMode(PIN_ETH_INT, INPUT);

  andonTowerAllOff();
  andonApplyMachineByte(ANDON_RX_INIT);
}

void loop()
{
  andonServicePressure();
  // TCP :8768 pendiente — al recibir ANDON_RX_* → andonApplyMachineByte()
}
