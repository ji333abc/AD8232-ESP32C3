#ifndef ECG_SIM_H
#define ECG_SIM_H
#include <stdint.h>
#define ECG_SIM_BPM 72U
#define ECG_SIM_PERIOD_MS (60000U / ECG_SIM_BPM)
#define ECG_SIM_STEP_MS 10U
/* Synthetic display amplitude in pixels, not calibrated millivolts. */
int16_t ECG_SimSample(uint32_t time_ms);
#endif
