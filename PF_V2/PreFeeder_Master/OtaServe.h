#pragma once

#include <WebServer.h>

// Sirve /ota/* desde LittleFS (carpeta data/ota/ → upload "ESP32 LittleFS").
bool otaServeInit();
void otaServeRegister(WebServer& server);
