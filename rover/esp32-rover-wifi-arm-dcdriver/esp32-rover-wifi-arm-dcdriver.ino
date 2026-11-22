#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>

// ============================================================
// WIFI ACCESS POINT
// ============================================================

const char* AP_SSID = "MobileManipulator";
const char* AP_PASS = "YOUR_AP_PASSWORD";  // set before flashing; keep out of git

WiFiUDP udp;
const uint16_t UDP_PORT = 4210;

// ============================================================
// AUTHENTICATION
// Must match the controller
// ============================================================

// Change this in BOTH sketches; they must be identical.
const uint32_t AUTH_TOKEN = 0x5EEDC0DE;

// ============================================================
// ROVER PIN CONFIGURATION
// ============================================================

// L298N motor driver
#define IN1 4
#define IN2 5
#define IN3 6
#define IN4 7

// Robotic arm servos
#define SERVO_BASE       13
#define SERVO_SHOULDER  14
#define SERVO_ELBOW      8
#define SERVO_WRIST      9
#define SERVO_GRIPPER   10

// ============================================================
// SERVO OBJECTS
// ============================================================

Servo baseServo;
Servo shoulderServo;
Servo elbowServo;
Servo wristServo;
Servo gripperServo;

// ============================================================
// UDP COMMAND PACKET
// This structure MUST match the controller exactly.
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

static_assert(sizeof(CommandPacket) == 17,
              "CommandPacket size mismatch!");

// ============================================================
// DRIVE COMMANDS
// Must match the controller
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
// ARM MODES
// ============================================================
//
// Mode 0: Base + Shoulder
// Mode 1: Elbow + Wrist
//
// ============================================================

enum ArmMode
{
  ARM_BASE_SHOULDER = 0,
  ARM_ELBOW_WRIST = 1
};

// ============================================================
// INITIAL SERVO POSITIONS
// ============================================================

uint8_t baseAngle = 90;
uint8_t shoulderAngle = 90;
uint8_t elbowAngle = 90;
uint8_t wristAngle = 90;

// Gripper: 0 = OPEN, 90 = CLOSED
uint8_t gripperAngle = 0;

// ============================================================
// WATCHDOG AND SESSION MANAGEMENT
// ============================================================

// Rover stops if no valid packet is received for 300 ms.
const uint32_t COMMAND_TIMEOUT = 300;

uint32_t lastValidPacket = 0;

uint32_t currentSession = 0;
uint32_t lastSequence = 0;

bool haveSession = false;
bool robotEnabled = false;

// ============================================================
// MOTOR FUNCTIONS
// Based on your actual motor test
// ============================================================

void stopMotors()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void moveForward()
{
  // IN1 HIGH, IN2 LOW
  // IN3 LOW,  IN4 HIGH

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void moveBackward()
{
  // IN1 LOW,  IN2 HIGH
  // IN3 HIGH, IN4 LOW

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);

  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void turnRight()
{
  // Right-turn movement from your motor test

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void turnLeft()
{
  // Left-turn movement from your motor test

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

// ============================================================
// EXECUTE DRIVE COMMAND
// ============================================================

void executeDrive(int8_t drive)
{
  switch (drive)
  {
    case DRIVE_FORWARD:
      moveForward();
      break;

    case DRIVE_BACKWARD:
      moveBackward();
      break;

    case DRIVE_LEFT:
      turnLeft();
      break;

    case DRIVE_RIGHT:
      turnRight();
      break;

    case DRIVE_STOP:
    default:
      stopMotors();
      break;
  }
}

// ============================================================
// SERVO ANGLE LIMITING
// ============================================================

uint8_t limitAngle(uint8_t angle, uint8_t maximum)
{
  if (angle > maximum)
  {
    return maximum;
  }

  return angle;
}

// ============================================================
// EXECUTE ARM COMMAND
// ============================================================

void executeArm(const CommandPacket& cmd)
{
  uint8_t angleX = limitAngle(cmd.armX, 180);
  uint8_t angleY = limitAngle(cmd.armY, 180);

  if (cmd.mode == ARM_BASE_SHOULDER)
  {
    // MODE 0: BASE + SHOULDER

    baseAngle = angleX;
    shoulderAngle = angleY;

    baseServo.write(baseAngle);
    shoulderServo.write(shoulderAngle);
  }
  else if (cmd.mode == ARM_ELBOW_WRIST)
  {
    // MODE 1: ELBOW + WRIST

    elbowAngle = angleX;
    wristAngle = angleY;

    elbowServo.write(elbowAngle);
    wristServo.write(wristAngle);
  }
}

// ============================================================
// EXECUTE GRIPPER COMMAND
// ============================================================
//
// 0 degrees  = OPEN
// 90 degrees = CLOSED
//
// ============================================================

void executeGripper(uint8_t angle)
{
  gripperAngle = limitAngle(angle, 90);

  gripperServo.write(gripperAngle);
}

// ============================================================
// COMMAND VALIDATION
// ============================================================

bool validCommand(const CommandPacket& cmd)
{
  if (cmd.drive < DRIVE_STOP || cmd.drive > DRIVE_RIGHT)
  {
    return false;
  }

  if (cmd.mode > ARM_ELBOW_WRIST)
  {
    return false;
  }

  return true;
}

// ============================================================
// SEQUENCE NUMBER VALIDATION
// Supports uint32_t rollover
// ============================================================

bool isNewerSequence(uint32_t incoming, uint32_t previous)
{
  return static_cast<int32_t>(incoming - previous) > 0;
}

// ============================================================
// FAILSAFE
// ============================================================
//
// On communication loss:
//
// 1. Stop rover motors.
// 2. Keep arm at its last commanded position.
// 3. Keep gripper at its last commanded position.
// 4. Wait for a valid controller packet.
//
// Servos are NOT detached.
//
// ============================================================

void disableRobot()
{
  stopMotors();

  robotEnabled = false;

  Serial.println();
  Serial.println("!!! FAILSAFE ACTIVATED !!!");
  Serial.println("Wi-Fi controller communication lost.");
  Serial.println("Motors stopped.");
  Serial.println("Arm and gripper holding position.");
}

// ============================================================
// DISCARD UNEXPECTED UDP PACKET
// ============================================================

void discardPacket()
{
  while (udp.available())
  {
    udp.read();
  }
}

// ============================================================
// HANDLE RECEIVED UDP PACKET
// ============================================================

void handlePacket()
{
  int packetSize = udp.parsePacket();

  if (packetSize <= 0)
  {
    return;
  }

  // ----------------------------------------------------------
  // CHECK PACKET SIZE
  // ----------------------------------------------------------

  if (packetSize != static_cast<int>(sizeof(CommandPacket)))
  {
    Serial.print("Rejected malformed packet. Size: ");
    Serial.println(packetSize);

    discardPacket();
    return;
  }

  CommandPacket cmd;

  int bytesRead = udp.read(
    reinterpret_cast<uint8_t*>(&cmd),
    sizeof(CommandPacket)
  );

  if (bytesRead != sizeof(CommandPacket))
  {
    Serial.println("ERROR: Incomplete UDP packet.");
    return;
  }

  // ----------------------------------------------------------
  // AUTHENTICATION
  // ----------------------------------------------------------

  if (cmd.auth != AUTH_TOKEN)
  {
    Serial.println("Rejected unauthorized packet.");
    return;
  }

  // ----------------------------------------------------------
  // VALIDATE COMMAND
  // ----------------------------------------------------------

  if (!validCommand(cmd))
  {
    Serial.println("Rejected invalid command.");
    return;
  }

  // ----------------------------------------------------------
  // SESSION MANAGEMENT
  // ----------------------------------------------------------

  bool newSession =
    (!haveSession || cmd.session != currentSession);

  if (newSession)
  {
    currentSession = cmd.session;
    lastSequence = cmd.sequence;
    haveSession = true;

    Serial.println("New controller session accepted.");
  }
  else
  {
    // --------------------------------------------------------
    // SEQUENCE CHECK
    // Reject duplicate or older packets
    // --------------------------------------------------------

    if (!isNewerSequence(cmd.sequence, lastSequence))
    {
      return;
    }

    lastSequence = cmd.sequence;
  }

  // ----------------------------------------------------------
  // VALID COMMAND RECEIVED
  // ----------------------------------------------------------

  lastValidPacket = millis();
  robotEnabled = true;

  // ----------------------------------------------------------
  // DRIVE
  // ----------------------------------------------------------

  executeDrive(cmd.drive);

  // ----------------------------------------------------------
  // ARM
  // ----------------------------------------------------------

  executeArm(cmd);

  // ----------------------------------------------------------
  // GRIPPER
  // ----------------------------------------------------------

  executeGripper(cmd.gripper);
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("    MOBILE MANIPULATOR ROVER");
  Serial.println("       Wi-Fi + UDP Receiver");
  Serial.println("======================================");

  // ----------------------------------------------------------
  // MOTOR INITIALIZATION
  // ----------------------------------------------------------

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  stopMotors();

  Serial.println("Motor pins initialized.");

  // ----------------------------------------------------------
  // SERVO INITIALIZATION
  // ----------------------------------------------------------

  baseServo.setPeriodHertz(50);
  shoulderServo.setPeriodHertz(50);
  elbowServo.setPeriodHertz(50);
  wristServo.setPeriodHertz(50);
  gripperServo.setPeriodHertz(50);

  baseServo.attach(SERVO_BASE, 500, 2500);
  shoulderServo.attach(SERVO_SHOULDER, 500, 2500);
  elbowServo.attach(SERVO_ELBOW, 500, 2500);
  wristServo.attach(SERVO_WRIST, 500, 2500);
  gripperServo.attach(SERVO_GRIPPER, 500, 2500);

  // Initial arm position
  baseServo.write(90);
  shoulderServo.write(90);
  elbowServo.write(90);
  wristServo.write(90);

  // Initial gripper position: OPEN
  gripperServo.write(0);

  Serial.println("Arm servos initialized.");
  Serial.println("Gripper initialized: OPEN.");

  // ----------------------------------------------------------
  // WI-FI ACCESS POINT
  // ----------------------------------------------------------

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  bool apOK = WiFi.softAP(
    AP_SSID,
    AP_PASS,
    1,      // Wi-Fi channel
    false,  // Broadcast SSID
    1       // Maximum connected stations
  );

  if (apOK)
  {
    Serial.println();
    Serial.println("Wi-Fi Access Point started.");

    Serial.print("SSID: ");
    Serial.println(AP_SSID);

    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());
  }
  else
  {
    Serial.println("ERROR: Wi-Fi AP failed!");
  }

  // ----------------------------------------------------------
  // UDP INITIALIZATION
  // ----------------------------------------------------------

  if (udp.begin(UDP_PORT))
  {
    Serial.print("UDP listening on port: ");
    Serial.println(UDP_PORT);
  }
  else
  {
    Serial.println("ERROR: UDP initialization failed!");
  }

  Serial.println();
  Serial.println("Waiting for controller...");
  Serial.println("Rover motors are STOPPED.");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
  // Receive and process controller commands
  handlePacket();

  // ----------------------------------------------------------
  // COMMUNICATION WATCHDOG
  // ----------------------------------------------------------

  if (robotEnabled &&
      millis() - lastValidPacket >= COMMAND_TIMEOUT)
  {
    disableRobot();
  }
}