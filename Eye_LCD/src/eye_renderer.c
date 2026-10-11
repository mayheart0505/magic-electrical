// Ported from eye_functions.ino + globals of ESP32LCDRound240x240Eyes.ino
// (Adafruit UncannyEyes by Phil Burgess, adapted by Bodmer; MIT license).
// Eye math, timing and compositing are unchanged. Only removed:
//   - TFT_eSPI output calls (replaced by lcd_push_rect)
//   - Arduino I/O and unused input paths (joystick, light sensor, buttons)
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"

#include "eye_arduino_shim.h"   // after system headers
#include "eye_display.h"
#include "eye_renderer.h"

static const char *TAG = "EYE";

// A pixel buffer is used during eye rendering. Must be whole rows.
#define BUFFER_SIZE 1024
static uint16_t pbuffer[BUFFER_SIZE];

// Blink state machine
#define NOBLINK 0
#define ENBLINK 1
#define DEBLINK 2
typedef struct {
  uint8_t  state;
  uint32_t duration;   // micros
  uint32_t startTime;  // micros
} eyeBlink;

#include "config.h"

_Static_assert(BUFFER_SIZE % SCREEN_WIDTH == 0, "BUFFER_SIZE must be whole rows");

static struct { eyeBlink blink; } eye[NUM_EYES];
static uint32_t startTime;

void frame(uint16_t iScale);
void split(int16_t startValue, int16_t endValue, uint32_t startTime_, int32_t duration, int16_t range);
void drawEye(uint8_t e, uint32_t iScale, uint32_t scleraX, uint32_t scleraY, uint32_t uT, uint32_t lT);

// Autonomous iris
static uint16_t oldIris = (IRIS_MIN + IRIS_MAX) / 2, newIris;

void eyes_init(void)
{
  ESP_LOGI(TAG, "Initialise eye objects");
  for (uint8_t e = 0; e < NUM_EYES; e++) eye[e].blink.state = NOBLINK;
  startTime = millis();
}

void updateEye(void)
{
  newIris = random(IRIS_MIN, IRIS_MAX);
  split(oldIris, newIris, micros(), 10000000L, IRIS_MAX - IRIS_MIN);
  oldIris = newIris;
}

void eye_draw_static(void)
{
  // mid iris, eye centred, lids fully open
  drawEye(0, (IRIS_MIN + IRIS_MAX) / 2,
          (SCLERA_WIDTH - SCREEN_WIDTH) / 2, (SCLERA_HEIGHT - SCREEN_HEIGHT) / 2, 0, 0);
}

// EYE-RENDERING FUNCTION --------------------------------------------------
void drawEye(
  uint8_t  e,
  uint32_t iScale,
  uint32_t scleraX,
  uint32_t scleraY,
  uint32_t uT,
  uint32_t lT) {

  uint32_t screenX, screenY, scleraXsave;
  int32_t  irisX, irisY;
  uint32_t p, a;
  uint32_t d;

  uint32_t pixels = 0;

  // ESP-IDF: eye is centred on the 240x240 panel; each flush is a
  // full-width band of whole rows pushed with draw_bitmap.
  const int EYE_X = (EYE_PANEL_W - SCREEN_WIDTH)  / 2;
  const int EYE_Y = (EYE_PANEL_H - SCREEN_HEIGHT) / 2;
  uint32_t  bandY = 0;

  scleraXsave = scleraX;
  irisY       = scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;

  uint16_t lidX = 0;
  uint16_t dlidX = (uint16_t)-1;
  if (e) dlidX = 1;
  for (screenY = 0; screenY < SCREEN_HEIGHT; screenY++, scleraY++, irisY++) {
    scleraX = scleraXsave;
    irisX   = scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
    if (e) lidX = 0; else lidX = SCREEN_WIDTH - 1;
    for (screenX = 0; screenX < SCREEN_WIDTH; screenX++, scleraX++, irisX++, lidX += dlidX) {
      if ((pgm_read_byte(lower + screenY * SCREEN_WIDTH + lidX) <= lT) ||
          (pgm_read_byte(upper + screenY * SCREEN_WIDTH + lidX) <= uT)) {  // Covered by eyelid
        p = 0;
      } else if ((irisY < 0) || (irisY >= IRIS_HEIGHT) ||
                 (irisX < 0) || (irisX >= IRIS_WIDTH)) {                   // In sclera
        p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX);
      } else {                                                             // Maybe iris...
        p = pgm_read_word(polar + irisY * IRIS_WIDTH + irisX);             // Polar angle/dist
        d = (iScale * (p & 0x7F)) / 128;                                   // Distance (Y)
        if (d < IRIS_MAP_HEIGHT) {                                         // Within iris area
          a = (IRIS_MAP_WIDTH * (p >> 7)) / 512;                           // Angle (X)
          p = pgm_read_word(iris + d * IRIS_MAP_WIDTH + a);                // Pixel = iris
        } else {                                                           // Not in iris
          p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX);    // Pixel = sclera
        }
      }
      pbuffer[pixels++] = p >> 8 | p << 8;   // byte swap: high byte first on the wire

      if (pixels >= BUFFER_SIZE) {
        uint32_t rows = BUFFER_SIZE / SCREEN_WIDTH;
        lcd_push_rect(EYE_X, EYE_Y + bandY, SCREEN_WIDTH, rows, pbuffer);
        bandY += rows;
        pixels = 0;
      }
    }
  }

  if (pixels) {
    lcd_push_rect(EYE_X, EYE_Y + bandY, SCREEN_WIDTH, pixels / SCREEN_WIDTH, pbuffer);
  }
}

// EYE ANIMATION -----------------------------------------------------------

static const uint8_t ease[] = { // Ease in/out curve for eye movements 3*t^2-2*t^3
  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  2,  2,  2,  3,
  3,  3,  4,  4,  4,  5,  5,  6,  6,  7,  7,  8,  9,  9, 10, 10,
  11, 12, 12, 13, 14, 15, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23,
  24, 25, 26, 27, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39,
  40, 41, 42, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 56, 57, 58,
  60, 61, 62, 63, 65, 66, 67, 69, 70, 72, 73, 74, 76, 77, 78, 80,
  81, 83, 84, 85, 87, 88, 90, 91, 93, 94, 96, 97, 98, 100, 101, 103,
  104, 106, 107, 109, 110, 112, 113, 115, 116, 118, 119, 121, 122, 124, 125, 127,
  128, 130, 131, 133, 134, 136, 137, 139, 140, 142, 143, 145, 146, 148, 149, 151,
  152, 154, 155, 157, 158, 159, 161, 162, 164, 165, 167, 168, 170, 171, 172, 174,
  175, 177, 178, 179, 181, 182, 183, 185, 186, 188, 189, 190, 192, 193, 194, 195,
  197, 198, 199, 201, 202, 203, 204, 205, 207, 208, 209, 210, 211, 213, 214, 215,
  216, 217, 218, 219, 220, 221, 222, 224, 225, 226, 227, 228, 228, 229, 230, 231,
  232, 233, 234, 235, 236, 237, 237, 238, 239, 240, 240, 241, 242, 243, 243, 244,
  245, 245, 246, 246, 247, 248, 248, 249, 249, 250, 250, 251, 251, 251, 252, 252,
  252, 253, 253, 253, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255
};

// Standby blink timing: gap between blinks = MIN + random(0..RAND), in microseconds.
// Defaults give a blink every 2 to 6 seconds.
#define STANDBY_BLINK_MIN_GAP_US  2000000
#define STANDBY_BLINK_RAND_US     4000000

static uint32_t timeOfLastBlink = 0L, timeToNextBlink = 0L;
static volatile eye_mode_t s_mode = EYE_MODE_STANDBY;

void eye_set_mode(eye_mode_t mode)
{
  // Coming back to standby: wait a normal gap before the first blink
  if (mode == EYE_MODE_STANDBY && s_mode != EYE_MODE_STANDBY) timeOfLastBlink = micros();
  s_mode = mode;
}

// Shake mode: gaze darts around rapidly, as if the eye is being shaken.
// Normal: moves last 72-144 ms, pauses 0-3 s. Shaking: values below (microseconds).
#define SHAKE_MOVE_MIN_US   25000
#define SHAKE_MOVE_MAX_US   60000
#define SHAKE_PAUSE_MAX_US  40000

static volatile bool s_shake = false;

void eye_set_shake(bool shaking)
{
  s_shake = shaking;
}

// Process motion for a single frame
void frame(uint16_t iScale) // Iris scale (0-1023)
{
  static uint32_t frames   = 0;
  static uint8_t  eyeIndex = 0;
  int16_t         eyeX, eyeY;
  uint32_t        t = micros();

  if (!(++frames & 255)) { // Every 256 frames...
    float elapsed = (millis() - startTime) / 1000.0f;
    if (elapsed > 0) ESP_LOGI(TAG, "%u fps", (unsigned)(frames / elapsed));
  }

  if (++eyeIndex >= NUM_EYES) eyeIndex = 0;

  // Autonomous X/Y eye motion
  static bool     eyeInMotion = false;
  static int16_t  eyeOldX = 512, eyeOldY = 512, eyeNewX = 512, eyeNewY = 512;
  static uint32_t eyeMoveStartTime = 0L;
  static int32_t  eyeMoveDuration  = 0L;

  int32_t dt = t - eyeMoveStartTime;
  if (eyeInMotion) {
    if (dt >= eyeMoveDuration) {
      eyeInMotion      = false;
      eyeMoveDuration  = s_shake ? random(SHAKE_PAUSE_MAX_US) : random(3000000); // 0-3 sec stop
      eyeMoveStartTime = t;
      eyeX = eyeOldX = eyeNewX;
      eyeY = eyeOldY = eyeNewY;
    } else {
      int16_t e = ease[255 * dt / eyeMoveDuration] + 1;
      eyeX = eyeOldX + (((eyeNewX - eyeOldX) * e) / 256);
      eyeY = eyeOldY + (((eyeNewY - eyeOldY) * e) / 256);
    }
  } else {
    eyeX = eyeOldX;
    eyeY = eyeOldY;
    // While shaking, cut any long pause that was already in progress short
    int32_t pauseLimit = eyeMoveDuration;
    if (s_shake && pauseLimit > SHAKE_PAUSE_MAX_US) pauseLimit = SHAKE_PAUSE_MAX_US;
    if (dt > pauseLimit) {
      int16_t  dx, dy;
      uint32_t d;
      do {
        eyeNewX = random(1024);
        eyeNewY = random(1024);
        dx      = (eyeNewX * 2) - 1023;
        dy      = (eyeNewY * 2) - 1023;
      } while ((d = (dx * dx + dy * dy)) > (1023 * 1023));
      eyeMoveDuration  = s_shake ? random(SHAKE_MOVE_MIN_US, SHAKE_MOVE_MAX_US)
                                 : random(72000, 144000); // ~1/14 - ~1/7 sec
      eyeMoveStartTime = t;
      eyeInMotion      = true;
    }
  }

  // Blinking
#ifdef AUTOBLINK
  if (s_mode == EYE_MODE_STANDBY && (t - timeOfLastBlink) >= timeToNextBlink) {
    timeOfLastBlink = t;
    uint32_t blinkDuration = random(36000, 72000); // ~1/28 - ~1/14 sec
    for (uint8_t e = 0; e < NUM_EYES; e++) {
      if (eye[e].blink.state == NOBLINK) {
        eye[e].blink.state     = ENBLINK;
        eye[e].blink.startTime = t;
        eye[e].blink.duration  = blinkDuration;
      }
    }
    timeToNextBlink = STANDBY_BLINK_MIN_GAP_US + random(STANDBY_BLINK_RAND_US);
  }
#endif

  if (eye[eyeIndex].blink.state) { // Eye currently blinking?
    if ((t - eye[eyeIndex].blink.startTime) >= eye[eyeIndex].blink.duration) {
      // (original "hold closed while button pressed" branch removed: no buttons)
      if (++eye[eyeIndex].blink.state > DEBLINK) {
        eye[eyeIndex].blink.state = NOBLINK;
      } else {
        eye[eyeIndex].blink.duration *= 2; // DEBLINK is 1/2 ENBLINK speed
        eye[eyeIndex].blink.startTime = t;
      }
    }
  }

  // Scale eye X/Y positions (0-1023) to pixel units used by drawEye()
  eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH  - 128);
  eyeY = map(eyeY, 0, 1023, 0, SCLERA_HEIGHT - 128);

  if (eyeX > (SCLERA_WIDTH - 128)) eyeX = (SCLERA_WIDTH - 128);

  static uint8_t uThreshold = 128;
  uint8_t        lThreshold, n;
#ifdef TRACKING
  int16_t sampleX = SCLERA_WIDTH  / 2 - (eyeX / 2),
          sampleY = SCLERA_HEIGHT / 2 - (eyeY + IRIS_HEIGHT / 4);
  if (sampleY < 0) n = 0;
  else            n = (pgm_read_byte(upper + sampleY * SCREEN_WIDTH + sampleX) +
                       pgm_read_byte(upper + sampleY * SCREEN_WIDTH + (SCREEN_WIDTH - 1 - sampleX))) / 2;
  uThreshold = (uThreshold * 3 + n) / 4;
  lThreshold = 254 - uThreshold;
#else
  uThreshold = lThreshold = 0;
#endif

  if (eye[eyeIndex].blink.state) {
    uint32_t s = (t - eye[eyeIndex].blink.startTime);
    if (s >= eye[eyeIndex].blink.duration) s = 255;
    else s = 255 * s / eye[eyeIndex].blink.duration;
    s          = (eye[eyeIndex].blink.state == DEBLINK) ? 1 + s : 256 - s;
    n          = (uThreshold * s + 254 * (257 - s)) / 256;
    lThreshold = (lThreshold * s + 254 * (257 - s)) / 256;
  } else {
    n          = uThreshold;
  }

  drawEye(eyeIndex, iScale, eyeX, eyeY, n, lThreshold);
  // (user_loop() hook removed)
}

// AUTONOMOUS IRIS SCALING -------------------------------------------------
void split(
  int16_t  startValue,
  int16_t  endValue,
  uint32_t startTime_,
  int32_t  duration,
  int16_t  range) {

  if (range >= 8) {
    range    /= 2;
    duration /= 2;
    int16_t  midValue = (startValue + endValue - range) / 2 + random(range);
    uint32_t midTime  = startTime_ + duration;
    split(startValue, midValue, startTime_, duration, range);
    split(midValue  , endValue, midTime  , duration, range);
  } else {
    int32_t dt;
    int16_t v;
    while ((dt = (micros() - startTime_)) < duration) {
      v = startValue + (((endValue - startValue) * dt) / duration);
      if (v < IRIS_MIN)      v = IRIS_MIN;
      else if (v > IRIS_MAX) v = IRIS_MAX;
      frame(v);
    }
  }
}