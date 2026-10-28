#include "battery_curve.h"

#include <stddef.h>

struct CurvePoint {
  uint32_t mv;
  int percent;
};

// Urut menurun berdasarkan tegangan.
static const CurvePoint kLipoCurve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
    {3910, 65},  {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
    {3770, 30},  {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},  {3270, 0},
};

int battery_percent_from_mv(uint32_t battery_mv) {
  const size_t n = sizeof(kLipoCurve) / sizeof(kLipoCurve[0]);
  if (battery_mv >= kLipoCurve[0].mv) return 100;
  if (battery_mv <= kLipoCurve[n - 1].mv) return 0;

  for (size_t i = 1; i < n; i++) {
    const CurvePoint &hi = kLipoCurve[i - 1];
    const CurvePoint &lo = kLipoCurve[i];
    if (battery_mv >= lo.mv) {
      uint32_t span_mv = hi.mv - lo.mv;
      int span_pct = hi.percent - lo.percent;
      return lo.percent + (int)(((battery_mv - lo.mv) * (uint32_t)span_pct + span_mv / 2) / span_mv);
    }
  }
  return 0;
}
