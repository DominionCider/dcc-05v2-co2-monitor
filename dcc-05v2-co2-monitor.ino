#include <ESP8266WiFi.h>
#include "mhz19.h"
#include "SoftwareSerial.h"

// Define these in the config.h file
//#define WIFI_SSID "yourwifi"
//#define WIFI_PASSWORD "yourpassword"
//#define INFLUX_HOSTNAME "data.example.com"
//#define INFLUX_PORT 8086
//#define INFLUX_PATH "/write?db=<database>&u=<user>&p=<pass>"
//#define WEBSERVER_USERNAME "something"
//#define WEBSERVER_PASSWORD "something"
#include "config.h"

#define DEVICE_NAME "dcc-05v2-co2-monitor"

#define PIN_RX  4
#define PIN_TX  5

SoftwareSerial sensor(PIN_RX, PIN_TX);


#include "libdcc/webserver.h"
#include "libdcc/influx.h"


void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  server.on("/restart", handleRestart);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
}


unsigned long lastIteration;
void loop() {
  server.handleClient();
  delay(100);

  if (millis() < lastIteration + 10000) return;
  lastIteration = millis();

  String sensorBody = String(DEVICE_NAME) + " uptime=" + String(millis()) + "i";

  int co2, temp;
  if (read_temp_co2(&sensor, &co2, &temp)) {
    Serial.print("CO2:");
    Serial.println(co2, DEC);
    sensorBody += String(",co2ppm=") + co2;
    Serial.print("TEMP:");
    Serial.println(temp, DEC);
    sensorBody += String(",temp=") + temp;
  } else {
    Serial.println("failed to read CO2 sensor!");
  }

  Serial.println(sensorBody);

  if (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to wifi...");
    return;
  }
  Serial.println("Wifi connected to " + WiFi.SSID() + " IP:" + WiFi.localIP().toString());

  WiFiClient client;
  if (client.connect(INFLUX_HOSTNAME, INFLUX_PORT)) {
    Serial.println(String("Connected to ") + INFLUX_HOSTNAME + ":" + INFLUX_PORT);
    delay(50);

    postRequest(sensorBody, client);

    client.stop();
  } else {
  }
  delay(100);
}



