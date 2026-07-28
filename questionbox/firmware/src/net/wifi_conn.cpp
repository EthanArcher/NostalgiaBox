#include "wifi_conn.h"
#include "device_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include "lwip/dns.h"

// The device's DHCP-provided DNS was intermittently failing to resolve the
// server. Force reliable public DNS servers (Google + Cloudflare).
static void set_public_dns()
{
  ip_addr_t d1;
  ip_addr_t d2;
  IP_ADDR4(&d1, 8, 8, 8, 8);
  IP_ADDR4(&d2, 1, 1, 1, 1);
  dns_setserver(0, &d1);
  dns_setserver(1, &d2);
}

bool WiFiConn_IsConnected()
{
  return WiFi.status() == WL_CONNECTED;
}

bool WiFiConn_Connect(uint32_t timeout_ms)
{
  if (sizeof(WIFI_SSID) <= 1) {
    Serial.println("[wifi] WIFI_SSID is empty - create include/secrets.h from the example");
    return false;
  }
  if (WiFiConn_IsConnected()) return true;

  Serial.printf("[wifi] connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
    delay(250);
  }

  if (WiFiConn_IsConnected()) {
    WiFi.setSleep(false);        // power-save was causing intermittent DNS/connection drops
    set_public_dns();
    Serial.printf("[wifi] connected, IP = %s (DNS 8.8.8.8/1.1.1.1)\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[wifi] connection FAILED");
  return false;
}
