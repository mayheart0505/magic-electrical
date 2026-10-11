#pragma once
#include <stdbool.h>
typedef enum { EYE_MODE_STANDBY, EYE_MODE_ANSWERING } eye_mode_t;

void eyes_init(void);
void updateEye(void);        // runs one iris-motion segment (many frames); call in a loop
void eye_draw_static(void);  // one fixed, fully-open, centered frame for testing
void eye_set_mode(eye_mode_t mode);
void eye_set_shake(bool shaking);   // true = gaze darts around rapidly