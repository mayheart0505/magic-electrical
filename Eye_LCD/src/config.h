
#include "data/naugaEye.h"   // sclera/iris/polar/upper/lower tables + SCREEN_*/IRIS_* defines

#define NUM_EYES 1

#define TRACKING    // eyelid tracks pupil (commented out = tracking OFF)
#define AUTOBLINK   // autonomous blinking

#if !defined(IRIS_MIN)
  #define IRIS_MIN  90
#endif
#if !defined(IRIS_MAX)
  #define IRIS_MAX 130
#endif