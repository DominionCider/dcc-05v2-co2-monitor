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

#define PIN_RX  0
#define PIN_TX  15
#define ONE_WIRE_PIN 2
#define SPEAKER_PIN 13
#define RED_LED_PIN 16
#define GREEN_LED_PIN 14

SoftwareSerial sensor(PIN_RX, PIN_TX);

#define N_SENSORS 2
byte sensorAddr[][8] = {
  {0x28, 0xFF, 0x23, 0xEB, 0xB2, 0x16, 0x05, 0xA9}, // (board)
  {0x28, 0xFF, 0x8F, 0x48, 0x01, 0x15, 0x04, 0xC9}  // (dongle)
};
char * sensorNames[] = {
  "board",
  "dongle"
};

#define SENSOR_FREQ 1000
#define UPLOAD_FREQ 10000


#define SETTINGS_VERSION "1mmj"
struct Settings {
  float co2Alart;       // Threshold to sound alarm
} settings = {
  2000
};

#include "libdcc/webserver.h"
#include "libdcc/onewire.h"
#include "libdcc/settings.h"
#include "libdcc/influx.h"
#include "libdcc/display.h"

String formatSettings() {
  return \
    String("co2Alart=") + String(settings.co2Alart, 3);
}

void handleSettings() {
  REQUIRE_AUTH;

  for (int i=0; i<server.args(); i++) {
    if (server.argName(i).equals("co2Alart")) {
      settings.co2Alart = server.arg(i).toFloat();
    } else {
      Serial.println("Unknown argument: " + server.argName(i) + ": " + server.arg(i));
    }
  }

  saveSettings();

  String msg = String("Settings saved: ") + formatSettings();
  Serial.println(msg);
  server.send(200, "text/plain", msg);

}

LiquidCrystal_I2C lcd(0x27, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);

unsigned long lastSensorIteration;
unsigned long lastUploadIteration;
int currentCo2;


void setup() {
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, HIGH);

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  server.on("/settings", handleSettings);
  server.on("/restart", handleRestart);
  server.on("/status", handleStatus);
  server.on("/sensors", handleSensors);
  server.onNotFound(handleNotFound);
  server.begin();

  loadSettings();
  Serial.println(formatSettings());

  Wire.begin(4, 5); // SDA=4, SCL=5
  lcd.begin(16, 2);
  lcd.backlight();
  loadCustomChars(lcd);

  lcd.home();
  lcd.print("    Dominion");
  lcd.setCursor(0, 1);
  lcd.print("    Cider Co.");
  delay(2000);

  lcd.setCursor(0, 1);
  lcd.print(String("CO") + char(4) + " Monitor v1.0");
  delay(2000);
  lcd.clear();

  // Initialize zeroth iteration
  takeAllMeasurementsAsync();
  lastSensorIteration = millis();
  // Offset upload events from sensor events
  lastUploadIteration = millis() + SENSOR_FREQ / 2;
}

WiFiClient client;

void loop() {
  server.handleClient();

  // Copy HTTP client response to Serial
  while (client.connected() && client.available()) {
    Serial.print(client.readStringUntil('\r'));
  }

  // If we are NOT ready to do a sensor iteration, return early
  if (millis() < lastSensorIteration + SENSOR_FREQ) {
    return;
  }

  // Read sensors
  float temp[N_SENSORS];
  float accum = 0.0;
  float numAccum = 0;
  String sensorBody = String(DEVICE_NAME) + " uptime=" + String(millis()) + "i";
  for (int i=0; i<N_SENSORS; i++) {
    Serial.print("Temperature sensor ");
    Serial.print(i);
    Serial.print(": ");
    if (readTemperature(sensorAddr[i], &temp[i])) {
      Serial.print(temp[i]);
      Serial.println();
      sensorBody += String(",") + sensorNames[i] + "=" + String(temp[i], 3);
    } else {
      temp[i] = NAN;
    }
  }

  // Append values from MH-Z19
  int mhzTemp;
  if (read_temp_co2(&sensor, &currentCo2, &mhzTemp)) {
    Serial.print("CO2:");
    Serial.println(currentCo2, DEC);
    sensorBody += String(",co2ppm=") + currentCo2;
    Serial.print("TEMP:");
    Serial.println(mhzTemp, DEC);
    sensorBody += String(",z19Temp=") + mhzTemp;
  } else {
    Serial.println("failed to read CO2 sensor!");
  }

  Serial.println(sensorBody);

  // Instruct sensors to take measurements for next iteration
  takeAllMeasurementsAsync();

  // Update Display
  lcd.setCursor(0, 0);
  lcd.print(String("CO") + char(4) + ": " + leftPad(String(currentCo2), 4) + " PPM");
  lcd.setCursor(0, 1);
  lcd.print("Temp: " + leftPad(String(temp[1], 1), 4) + char(3) + "C");

  if (WiFi.status() == WL_CONNECTED) {
    lcd.setCursor(15, 0);
    lcd.print(char(0));
  } else {
    lcd.setCursor(15, 0);
    lcd.print(char(1));
  }

  lastSensorIteration = millis();

  // Check alarm state
  if (currentCo2 > settings.co2Alart) {
    tone(SPEAKER_PIN, 450);
    lcd.noBacklight();
    digitalWrite(RED_LED_PIN, HIGH);

    delay(25);
    tone(SPEAKER_PIN, 900);
    delay(25);
    tone(SPEAKER_PIN, 2560);
    delay(25);

    noTone(SPEAKER_PIN);
    lcd.backlight();
    digitalWrite(RED_LED_PIN, LOW);
  }

  // If we are ready to do an upload iteration, do that now
  if (millis() > lastUploadIteration + UPLOAD_FREQ) {
    Serial.println("Wifi connected to " + WiFi.SSID() + " IP:" + WiFi.localIP().toString());
    client.connect(INFLUX_HOSTNAME, INFLUX_PORT);
    postRequestAsync(sensorBody, client);
    lastUploadIteration = millis();
  }

}



