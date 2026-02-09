#include <Arduino.h>
#include <M5Unified.h>
#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <FastLED.h>
#include "SD.h"
#include <vector>
#include <esp_timer.h>

#define LED_INTENSITY 70

#define PIN_SCL 15 // Grove on board, SL2
#define PIN_SDA 13 // Grove on board, SL2
#define PIN_SOL 1  // SL2
#define PIN_SW 3   // SL2
#define PIN_LED 43 // LED on board, SL2

#define PWM_STRONG_ON 255
#define PWM_WEAK_ON 25
#define PWM_OFF 0

#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

PN532_I2C pn532i2c(Wire);
PN532 nfc(pn532i2c);

std::vector<String> id_list;

static const uint16_t kUnauthorizedHoldMs = 1000;
static const uint16_t kLogHoldMs = 300;
static const uint16_t kCardCooldownMs = 2000;
static const uint32_t kUnlockHoldMs = 5000;

String last_uid = "";
uint64_t last_uid_ms = 0;
uint32_t boot_id = 0;
bool unlocking_active = false;
uint64_t unlock_start_ms = 0;

enum class InitError
{
	RFID_INIT = 1,
	SD_INIT = 2,
	BOOT_ID = 3,
	LOG_HEADER = 4,
	IDLIST = 5
};

void setLED(uint8_t r, uint8_t g, uint8_t b)
{
	// PL9823=RGB / WS2812=GRB
	leds[0] = CRGB(g, r, b);
	FastLED.show();
}

void blinkLED(uint8_t r, uint8_t g, uint8_t b, uint8_t times, uint16_t on_ms, uint16_t off_ms)
{
	for (uint8_t i = 0; i < times; i++)
	{
		setLED(r, g, b);
		delay(on_ms);
		setLED(0, 0, 0);
		delay(off_ms);
	}
}

bool isLocked()
{
	return (digitalRead(PIN_SW) == LOW);
}

void showLockStatusLED()
{
	if (isLocked())
	{
		// locked = green
		setLED(0, LED_INTENSITY, 0);
	}
	else
	{
		// unlocked = blue
		setLED(0, 0, LED_INTENSITY);
	}
}

void unlockSolenoid()
{
	analogWrite(PIN_SOL, PWM_STRONG_ON);
	delay(300);
	analogWrite(PIN_SOL, PWM_WEAK_ON);
}

void lockSolenoid()
{
	analogWrite(PIN_SOL, PWM_OFF);
}

bool initSD()
{
	Serial.println("[SD] init start");
	SPI.begin(5, 7, 9); // SCK, MISO, MOSI
	bool ok = SD.begin(44, SPI, 25000000);
	Serial.println(ok ? "[SD] init ok" : "[SD] init failed");
	return ok;
}

bool readIDListFromSD()
{
	Serial.println("[IDLIST] read start");
	id_list.clear();
	blinkLED(LED_INTENSITY, 0, LED_INTENSITY, 2, 120, 120); // purple blink

	File file = SD.open("/IDlist.csv", "r");
	if (!file)
	{
		Serial.println("[IDLIST] open failed");
		return false;
	}

	while (file.available())
	{
		String line = file.readStringUntil('\n');
		line.trim();
		if (line.length() == 0)
		{
			continue;
		}
		int comma = line.indexOf(',');
		String uid = (comma >= 0) ? line.substring(0, comma) : line;
		uid.trim();
		if (uid.length() > 0)
		{
			id_list.push_back(uid);
		}
	}
	file.close();
	Serial.print("[IDLIST] read ok, count=");
	Serial.println((int)id_list.size());
	return true;
}

bool isAuthorized(const String &uid)
{
	for (size_t i = 0; i < id_list.size(); i++)
	{
		if (id_list[i] == uid)
		{
			return true;
		}
	}
	return false;
}

String readCardUID()
{
	uint8_t uid[7];
	uint8_t uidLength = 0;
	bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50);
	if (!success || uidLength == 0)
	{
		return "";
	}

	String id = "";
	for (uint8_t i = 0; i < uidLength; i++)
	{
		if (uid[i] < 0x10)
		{
			id += "0";
		}
		id += String(uid[i], HEX);
	}
	return id;
}

bool recordLog(const String &uid)
{
	Serial.println("[LOG] write start");
	setLED(LED_INTENSITY, 0, LED_INTENSITY); // purple
	File file = SD.open("/log.csv", FILE_APPEND);
	if (!file)
	{
		Serial.println("[LOG] open failed");
		return false;
	}
	uint64_t ms = (uint64_t)esp_timer_get_time() / 1000ULL;
	String line = String(boot_id) + "," + String((uint32_t)ms) + "," + uid + "\n";
	file.print(line);
	file.close();
	delay(kLogHoldMs);
	Serial.print("[LOG] write ok: ");
	Serial.print(boot_id);
	Serial.print(",");
	Serial.print((uint32_t)ms);
	Serial.print(",");
	Serial.println(uid);
	return true;
}

uint32_t readBootId()
{
	uint32_t id = 0;
	File file = SD.open("/boot_id.txt", "r");
	if (file)
	{
		String line = file.readStringUntil('\n');
		line.trim();
		if (line.length() > 0)
		{
			id = (uint32_t)line.toInt();
		}
		file.close();
	}
	Serial.print("[BOOT] read id=");
	Serial.println(id);
	return id;
}

bool writeBootId(uint32_t id)
{
	File file = SD.open("/boot_id.txt", "w");
	if (!file)
	{
		Serial.println("[BOOT] write failed");
		return false;
	}
	file.print(String(id) + "\n");
	file.close();
	Serial.print("[BOOT] write ok id=");
	Serial.println(id);
	return true;
}

bool ensureLogHeader()
{
	Serial.println("[LOG] ensure header");
	File file = SD.open("/log.csv", "r");
	if (file)
	{
		bool hasData = file.size() > 0;
		file.close();
		if (hasData)
		{
			Serial.println("[LOG] header already exists");
			return true;
		}
	}

	File out = SD.open("/log.csv", FILE_APPEND);
	if (!out)
	{
		Serial.println("[LOG] header open failed");
		return false;
	}
	out.print("boot_id,elapsed_ms,uid\n");
	out.close();
	Serial.println("[LOG] header write ok");
	return true;
}

void initFailLoop(InitError err)
{
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
	uint8_t count = 1;

	switch (err)
	{
	case InitError::RFID_INIT:
		r = LED_INTENSITY;
		g = 0;
		b = 0;
		count = 1;
		break;
	case InitError::SD_INIT:
		r = LED_INTENSITY;
		g = 0;
		b = 0;
		count = 2;
		break;
	case InitError::BOOT_ID:
		r = LED_INTENSITY;
		g = 0;
		b = 0;
		count = 3;
		break;
	case InitError::LOG_HEADER:
		r = LED_INTENSITY;
		g = 0;
		b = LED_INTENSITY;
		count = 1;
		break;
	case InitError::IDLIST:
		r = LED_INTENSITY;
		g = 0;
		b = LED_INTENSITY;
		count = 2;
		break;
	default:
		r = LED_INTENSITY;
		g = 0;
		b = 0;
		count = 1;
		break;
	}

	while (true)
	{
		blinkLED(r, g, b, count, 150, 150);
		delay(1000);
	}
}

void setup()
{
	M5.begin();
	Serial.println("[SYS] boot");
	Wire.end();
	Wire.begin(PIN_SDA, PIN_SCL);

	FastLED.addLeds<NEOPIXEL, PIN_LED>(leds, NUM_LEDS);
	setLED(LED_INTENSITY, LED_INTENSITY, LED_INTENSITY); // white during init

	analogWrite(PIN_SOL, PWM_OFF);
	pinMode(PIN_SW, INPUT_PULLUP);

	nfc.begin();
	uint32_t versiondata = nfc.getFirmwareVersion();
	if (!versiondata)
	{
		Serial.println("[RFID] init failed");
		initFailLoop(InitError::RFID_INIT);
	}
	Serial.println("[RFID] init ok");
	nfc.SAMConfig();

	if (!initSD())
	{
		initFailLoop(InitError::SD_INIT);
	}

	boot_id = readBootId() + 1;
	if (!writeBootId(boot_id))
	{
		initFailLoop(InitError::BOOT_ID);
	}

	if (!ensureLogHeader())
	{
		initFailLoop(InitError::LOG_HEADER);
	}

	if (!readIDListFromSD())
	{
		initFailLoop(InitError::IDLIST);
	}

	showLockStatusLED();
}

void updateUnlockState()
{
	if (!unlocking_active)
	{
		return;
	}

	uint64_t now = (uint64_t)esp_timer_get_time() / 1000ULL;
	uint64_t elapsed = now - unlock_start_ms;

	if (elapsed < kUnlockHoldMs)
	{
		return; // keep power for first 5s unconditionally
	}

	if (!isLocked())
	{
		Serial.println("[SOL] unlock detected, power off");
		lockSolenoid();
		unlocking_active = false;
		return;
	}

	if (elapsed >= (kUnlockHoldMs * 2))
	{
		Serial.println("[SOL] still locked after 10s, power off");
		lockSolenoid();
		unlocking_active = false;
	}
}

void loop()
{
	M5.update();

	showLockStatusLED();
	updateUnlockState();

	String uid = readCardUID();
	if (uid.length() > 0)
	{
		Serial.print("[RFID] detected uid=");
		Serial.println(uid);
		uint64_t now = (uint64_t)esp_timer_get_time() / 1000ULL;
		if (uid == last_uid && (now - last_uid_ms) < kCardCooldownMs)
		{
			Serial.println("[RFID] ignored (cooldown)");
			delay(50);
			return;
		}
		last_uid = uid;
		last_uid_ms = now;

		if (isAuthorized(uid))
		{
			Serial.println("[AUTH] ok");
			unlockSolenoid();
			unlocking_active = true;
			unlock_start_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
			recordLog(uid);
		}
		else
		{
			Serial.println("[AUTH] failed");
			setLED(LED_INTENSITY, 0, 0); // red
			delay(kUnauthorizedHoldMs);
			lockSolenoid();
		}
	}
	delay(10);
}
