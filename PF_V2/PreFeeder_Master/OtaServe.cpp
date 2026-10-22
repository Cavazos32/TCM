#include "OtaServe.h"

#include <LittleFS.h>

static bool s_lfsOk = false;
static WebServer* s_http = nullptr;

static void handleOtaRequest()
{
  if (!s_http) return;
  if (!s_lfsOk)
  {
    s_http->send(503, "text/plain", "LittleFS no montado");
    return;
  }
  String uri = s_http->uri();
  if (!uri.startsWith("/ota/") || uri.indexOf("..") >= 0)
  {
    s_http->send(403, "text/plain", "forbidden");
    return;
  }
  if (!LittleFS.exists(uri))
  {
    s_http->send(404, "text/plain", "missing");
    return;
  }
  File f = LittleFS.open(uri, "r");
  if (!f)
  {
    s_http->send(500, "text/plain", "open fail");
    return;
  }
  const char* ct = "application/octet-stream";
  if (uri.endsWith(".json")) ct = "application/json";
  s_http->sendHeader("Cache-Control", "no-store");
  s_http->streamFile(f, ct);
  f.close();
}

bool otaServeInit()
{
  s_lfsOk = LittleFS.begin(true);
  if (!s_lfsOk)
  {
    Serial.println("[MASTER] LittleFS: no montado (OTA LAN /ota/ no disponible)");
    return false;
  }
  Serial.println("[MASTER] LittleFS OK — sirve http://<ip>/ota/ (manifest + .bin)");
  return true;
}

void otaServeRegister(WebServer& server)
{
  s_http = &server;
  server.on("/ota/pf_lr_L_version.json", HTTP_GET, handleOtaRequest);
  server.on("/ota/pf_lr_R_version.json", HTTP_GET, handleOtaRequest);
  server.on("/ota/PF_LR_L.bin", HTTP_GET, handleOtaRequest);
  server.on("/ota/PF_LR_R.bin", HTTP_GET, handleOtaRequest);
  server.on("/ota/PF_Master.bin", HTTP_GET, handleOtaRequest);
  server.on("/ota/pf_master_version.json", HTTP_GET, handleOtaRequest);
}
