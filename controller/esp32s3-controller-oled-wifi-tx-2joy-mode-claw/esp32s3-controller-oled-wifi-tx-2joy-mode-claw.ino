#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

// ============================================================
// WIFI
// ============================================================

const char* AP_SSID = "MobileManipulator";
const char* AP_PASS = "YOUR_AP_PASSWORD";  // set before flashing; keep out of git

// Rover ESP32 AP default IP
IPAddress roverIP(192, 168, 4, 1);

const uint16_t ROVER_PORT = 4210;
const uint16_t LOCAL_PORT = 4211;

WiFiUDP udp;

// ============================================================
// RGB
// ============================================================

#define RGB_PIN   48
#define RGB_COUNT 1

Adafruit_NeoPixel rgb(
  RGB_COUNT,
  RGB_PIN,
  NEO_GRB + NEO_KHZ800
);

// ============================================================
// OLED SSD1309 - HARDWARE SPI
// ============================================================

#define OLED_SCK  12
#define OLED_MOSI 11
#define OLED_CS   10
#define OLED_DC   9
#define OLED_RST  8

U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI display(
  U8G2_R0,
  OLED_CS,
  OLED_DC,
  OLED_RST
);

// ============================================================
// JOYSTICKS
// ============================================================

// Drive joystick
#define DRIVE_X 1
#define DRIVE_Y 2

// Arm joystick
#define ARM_X 5
#define ARM_Y 6

// ============================================================
// SWITCHES
// ============================================================

#define MODE_PIN 15
#define CLAW_PIN 16

// ============================================================
// AUTHENTICATION
// Must MATCH rover
// ============================================================

// Change this in BOTH sketches; they must be identical.
const uint32_t AUTH_TOKEN = 0x5EEDC0DE;

// ============================================================
// DRIVE COMMANDS
// ============================================================

enum DriveCommand
{
  DRIVE_STOP = 0,
  DRIVE_FORWARD,
  DRIVE_BACKWARD,
  DRIVE_LEFT,
  DRIVE_RIGHT
};

// ============================================================
// PACKET
// MUST MATCH ROVER EXACTLY
// ============================================================

#pragma pack(push, 1)

struct CommandPacket
{
  uint32_t auth;
  uint32_t session;
  uint32_t sequence;

  int8_t drive;

  uint8_t mode;

  uint8_t armX;
  uint8_t armY;

  uint8_t gripper;
};

#pragma pack(pop)

CommandPacket packet;

// ============================================================
// SESSION
// ============================================================

uint32_t sessionID;
uint32_t sequence = 0;

// ============================================================
// RGB HELPERS
// ============================================================

void setRGB(uint8_t r, uint8_t g, uint8_t b)
{
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
}

void showConnecting()
{
  // RED
  setRGB(255, 0, 0);
}

void showConnected()
{
  // BLUE
  setRGB(0, 0, 150);
}

void showSignalSent()
{
  // GREEN
  setRGB(0, 255, 0);
}

// ============================================================
// JOYSTICK → SERVO ANGLE
// ============================================================

int joystickToAngle(int value)
{
  value = constrain(value, 0, 4095);

  if (value < 1600)
  {
    return map(value, 0, 1600, 0, 80);
  }
  else if (value <= 2000)
  {
    return 90;
  }
  else
  {
    return map(value, 2000, 4095, 90, 180);
  }
}

// ============================================================
// DRIVE COMMAND
// ============================================================

uint8_t getDriveCommand(int x, int y)
{
  const int DEAD_LOW  = 1600;
  const int DEAD_HIGH = 2000;

  // FORWARD
  if (y < DEAD_LOW && x >= 1300 && x <= 2500)
  {
    return DRIVE_FORWARD;
  }

  // BACKWARD
  if (y > DEAD_HIGH && x >= 1300 && x <= 2500)
  {
    return DRIVE_BACKWARD;
  }

  // LEFT
  if (x < DEAD_LOW && y >= 1300 && y <= 2500)
  {
    return DRIVE_LEFT;
  }

  // RIGHT
  if (x > DEAD_HIGH && y >= 1300 && y <= 2500)
  {
    return DRIVE_RIGHT;
  }

  return DRIVE_STOP;
}

// ============================================================
// DRIVE TEXT
// ============================================================

const char* driveToString(uint8_t drive)
{
  switch (drive)
  {
    case DRIVE_FORWARD:
      return "FWD";

    case DRIVE_BACKWARD:
      return "BACK";

    case DRIVE_LEFT:
      return "LEFT";

    case DRIVE_RIGHT:
      return "RIGHT";

    default:
      return "STOP";
  }
}

// ============================================================
// OLED
// ============================================================

void updateDisplay(
  uint8_t drive,
  uint8_t mode,
  uint8_t armX,
  uint8_t armY,
  uint8_t gripper
)
{
  display.clearBuffer();

  display.setFont(u8g2_font_6x12_tf);

  // ----------------------------------------------------------
  // Header
  // ----------------------------------------------------------

  display.drawStr(0, 10, "MOBILE MANIP");

  if (WiFi.status() == WL_CONNECTED)
  {
    display.drawStr(76, 10, "WIFI OK");
  }
  else
  {
    display.drawStr(76, 10, "NO WIFI");
  }

  // ----------------------------------------------------------
  // Drive
  // ----------------------------------------------------------

  display.drawStr(0, 22, "DRV:");
  display.drawStr(30, 22, driveToString(drive));

  // ----------------------------------------------------------
  // Mode
  // ----------------------------------------------------------

  display.drawStr(70, 22, "MODE:");

  if (mode == 0)
  {
    display.drawStr(101, 22, "1");
  }
  else
  {
    display.drawStr(101, 22, "2");
  }

  // ----------------------------------------------------------
  // ARM
  // ----------------------------------------------------------

  display.drawStr(0, 34, "AX:");
  display.setCursor(20, 34);
  display.print(armX);

  display.drawStr(55, 34, "AY:");
  display.setCursor(75, 34);
  display.print(armY);

  // ----------------------------------------------------------
  // GRIPPER
  // ----------------------------------------------------------

  display.drawStr(0, 46, "GRIP:");
  display.setCursor(35, 46);
  display.print(gripper);

  display.drawStr(70, 46, "SEQ:");
  display.setCursor(95, 46);
  display.print(sequence);

  // ----------------------------------------------------------
  // Controller IP
  // ----------------------------------------------------------

  display.setCursor(0, 58);
  display.print("IP:");
  display.print(WiFi.localIP());

  display.sendBuffer();
}

// ============================================================
// WIFI CONNECT
// ============================================================

void connectToRover()
{
  Serial.println();
  Serial.println("Connecting to rover...");
  
  showConnecting();

  WiFi.mode(WIFI_STA);

  // Better latency for control
  WiFi.setSleep(false);

  WiFi.begin(AP_SSID, AP_PASS);

  uint32_t startTime = millis();

  while (WiFi.status() != WL_CONNECTED)
  {
    showConnecting();

    Serial.print(".");

    delay(200);

    if (millis() - startTime > 15000)
    {
      Serial.println();
      Serial.println("WiFi connection timeout");

      // Keep trying
      startTime = millis();
      WiFi.disconnect();
      delay(500);
      WiFi.begin(AP_SSID, AP_PASS);
    }
  }

  Serial.println();
  Serial.println("WiFi CONNECTED");

  Serial.print("Controller IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("Rover IP: ");
  Serial.println(roverIP);

  showConnected();
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println("==================================");
  Serial.println(" Mobile Manipulator Controller");
  Serial.println(" WiFi UDP Controller");
  Serial.println("==================================");

  // ----------------------------------------------------------
  // RGB
  // ----------------------------------------------------------

  rgb.begin();
  rgb.setBrightness(100);

  showConnecting();

  // ----------------------------------------------------------
  // OLED
  // ----------------------------------------------------------

  SPI.begin(
    OLED_SCK,
    -1,
    OLED_MOSI,
    OLED_CS
  );

  display.begin();

  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tf);

  display.drawStr(0, 15, "MOBILE MANIP");
  display.drawStr(0, 30, "Connecting...");
  display.sendBuffer();

  // ----------------------------------------------------------
  // ADC
  // ----------------------------------------------------------

  analogSetPinAttenuation(DRIVE_X, ADC_11db);
  analogSetPinAttenuation(DRIVE_Y, ADC_11db);

  analogSetPinAttenuation(ARM_X, ADC_11db);
  analogSetPinAttenuation(ARM_Y, ADC_11db);

  // ----------------------------------------------------------
  // SWITCHES
  // ----------------------------------------------------------

  pinMode(MODE_PIN, INPUT_PULLUP);
  pinMode(CLAW_PIN, INPUT_PULLUP);

  // ----------------------------------------------------------
  // SESSION
  // ----------------------------------------------------------

  sessionID = esp_random();

  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  connectToRover();

  // ----------------------------------------------------------
  // UDP
  // ----------------------------------------------------------

  udp.begin(LOCAL_PORT);

  Serial.print("UDP local port: ");
  Serial.println(LOCAL_PORT);

  // ----------------------------------------------------------
  // Final display
  // ----------------------------------------------------------

  updateDisplay(
    DRIVE_STOP,
    0,
    90,
    90,
    120
  );
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  // ==========================================================
  // WIFI LOST?
  // ==========================================================

  if (WiFi.status() != WL_CONNECTED)
  {
    showConnecting();

    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);

    display.drawStr(0, 15, "MOBILE MANIP");
    display.drawStr(0, 32, "WIFI LOST");
    display.drawStr(0, 48, "RECONNECTING");

    display.sendBuffer();

    connectToRover();
  }

  // ==========================================================
  // READ DRIVE JOYSTICK
  // ==========================================================

  int driveX = analogRead(DRIVE_X);
  int driveY = analogRead(DRIVE_Y);

  // ==========================================================
  // READ ARM JOYSTICK
  // ==========================================================

  int armXRaw = analogRead(ARM_X);
  int armYRaw = analogRead(ARM_Y);

  // ==========================================================
  // CONVERT ARM JOYSTICK
  // ==========================================================

  uint8_t armX = joystickToAngle(armXRaw);
  uint8_t armY = joystickToAngle(armYRaw);

  // ==========================================================
  // DRIVE
  // ==========================================================

  uint8_t drive = getDriveCommand(driveX, driveY);

  // ==========================================================
  // MODE
  //
  // LOW = MODE 1
  //      Base + Shoulder
  //
  // HIGH = MODE 2
  //       Elbow + Wrist
  // ==========================================================

  uint8_t mode;

  if (digitalRead(MODE_PIN) == LOW)
  {
    mode = 0;
  }
  else
  {
    mode = 1;
  }

  // ==========================================================
  // GRIPPER
  //
  // PRESSED  = 20° CLOSED
  // RELEASED = 120° OPEN
  // ==========================================================

  uint8_t gripper;

  if (digitalRead(CLAW_PIN) == LOW)
  {
    gripper = 20;
  }
  else
  {
    gripper = 120;
  }

  // ==========================================================
  // BUILD PACKET
  // ==========================================================

  packet.auth     = AUTH_TOKEN;
  packet.session  = sessionID;
  packet.sequence = sequence++;

  packet.drive = drive;

  packet.mode = mode;

  packet.armX = armX;
  packet.armY = armY;

  packet.gripper = gripper;

  // ==========================================================
  // SEND UDP
  // ==========================================================

  bool sent = false;

  if (WiFi.status() == WL_CONNECTED)
  {
    if (udp.beginPacket(roverIP, ROVER_PORT))
    {
      udp.write(
        (uint8_t*)&packet,
        sizeof(packet)
      );

      sent = udp.endPacket();
    }
  }

  // ==========================================================
  // RGB
  // ==========================================================

  if (sent)
  {
    // GREEN = signal sent
    showSignalSent();
  }
  else
  {
    // BLUE = connected but packet not sent
    if (WiFi.status() == WL_CONNECTED)
    {
      showConnected();
    }
    else
    {
      showConnecting();
    }
  }

  // ==========================================================
  // SERIAL
  // ==========================================================

  static uint32_t lastPrint = 0;

  if (millis() - lastPrint >= 250)
  {
    lastPrint = millis();

    Serial.println("--------------------------------");

    Serial.print("WiFi: ");
    Serial.println(
      WiFi.status() == WL_CONNECTED
        ? "CONNECTED"
        : "DISCONNECTED"
    );

    Serial.print("Drive X: ");
    Serial.println(driveX);

    Serial.print("Drive Y: ");
    Serial.println(driveY);

    Serial.print("Drive: ");
    Serial.println(driveToString(drive));

    Serial.print("Arm X: ");
    Serial.print(armXRaw);

    Serial.print(" -> ");
    Serial.print(armX);

    Serial.print(" | Arm Y: ");
    Serial.print(armYRaw);

    Serial.print(" -> ");
    Serial.println(armY);

    Serial.print("Mode: ");
    Serial.println(mode == 0 ? "BASE + SHOULDER" : "ELBOW + WRIST");

    Serial.print("Gripper: ");
    Serial.println(gripper);

    Serial.print("Sequence: ");
    Serial.println(packet.sequence);

    Serial.print("Packet: ");
    Serial.println(sent ? "SENT" : "FAILED");
  }

  // ==========================================================
  // OLED
  // ==========================================================

  static uint32_t lastDisplay = 0;

  if (millis() - lastDisplay >= 100)
  {
    lastDisplay = millis();

    updateDisplay(
      drive,
      mode,
      armX,
      armY,
      gripper
    );
  }

  // ==========================================================
  // 25 Hz CONTROL LOOP
  // ==========================================================

  delay(40);
}