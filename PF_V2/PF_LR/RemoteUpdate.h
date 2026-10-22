#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// OTA por HTTPS (version.json → .bin); la descarga corre en una tarea aparte.
class RemoteUpdate {
public:
  void begin(const char* currentVersion, const char* manifestUrl, uint32_t intervalMs, uint32_t bootDelayMs, uint32_t retryMs);

  // Llamar en cada loop(). No bloquea: decide si toca revisar y lanza la tarea.
  void loop();

  // Fuerza una revisión lo antes posible (desde la web). false si ya está ocupado.
  bool requestCheck();

  // true mientras se está escribiendo el firmware nuevo en flash.
  bool flashing() const { return _flashing; }
  bool busy() const { return _busy; }

  void statusJson(JsonObject obj);

  // Compara versiones "1.2.3" (acepta prefijo "v"). <0, 0, >0 como strcmp.
  static int compareVersions(const char* a, const char* b);

private:
  static void taskEntry(void* arg);
  void runCheck();
  bool fetchManifest(String& version, String& url, String& md5, String& sha256);
  void setStatus(const char* state, const char* message);

  const char* _currentVersion = "";
  const char* _manifestUrl = "";
  uint32_t _intervalMs = 0;
  uint32_t _bootDelayMs = 0;
  uint32_t _retryMs = 0;
  volatile uint32_t _nextCheckMs = 0;

  volatile bool _busy = false;
  volatile bool _flashing = false;
  volatile bool _forceRequested = false;
  volatile int _progress = 0;

  // Estado (protegido con _mux; se escribe desde la tarea y se lee desde la web).
  char _state[16] = "idle";
  char _message[96] = "";
  char _latest[24] = "";
  uint32_t _lastCheckMs = 0;
  bool _checkedOnce = false;
};

extern RemoteUpdate remoteUpdate;
