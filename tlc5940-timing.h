#ifndef TLC5940_TIMING_H
#define TLC5940_TIMING_H

#define GSCLK_SPEED_HZ  2500000
#define GSCLK_PERIOD_NS ((unsigned long) (1e9 / GSCLK_SPEED_HZ))
#define BLANK_PERIOD_NS (4096 * GSCLK_PERIOD_NS)

#endif /* TLC5940_TIMING_H */
