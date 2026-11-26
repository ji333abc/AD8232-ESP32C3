#include "ecg_sim.h"

int16_t ECG_SimSample(uint32_t time_ms)
{
  /* Piecewise-linear P/QRS/T-like shape for UI demonstration only. */
  static const struct { uint16_t phase; int16_t amplitude; } shape[] = {
    {0,0}, {100,0}, {145,7}, {190,0}, {270,0}, {295,-9},
    {320,55}, {345,-16}, {380,0}, {460,0}, {545,15}, {650,0}, {1000,0}
  };
  uint32_t phase = (time_ms % ECG_SIM_PERIOD_MS) * 1000U / ECG_SIM_PERIOD_MS;
  for (unsigned i = 1; i < sizeof shape / sizeof shape[0]; ++i)
  {
    if (phase <= shape[i].phase)
      return shape[i-1].amplitude +
          (int32_t)(shape[i].amplitude - shape[i-1].amplitude) *
          (int32_t)(phase - shape[i-1].phase) /
          (shape[i].phase - shape[i-1].phase);
  }
  return 0;
}
