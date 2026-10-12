#include <math.h>
#include <stdio.h>

#include "meter.h"

static int failed;

static void check(const char* name, int ok) {
    failed |= !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

static int near(float a, float b) {
    return fabsf(a - b) < 1e-4f;
}

int main(void) {
    int stepsOk = 1, fits = 1, replayAbove = 1;
    for (int option = 0; option < PREFILL_OPTIONS; option++) {
        uint32_t prefill = prefill_samples(option);
        int ms = PREFILL_MIN_MS + PREFILL_STEP_MS * option;
        stepsOk &= samples_to_ms(prefill) == ms;
        fits &= max_fill_samples(prefill) < RING_SIZE && max_fill_samples(prefill) > prefill;
        replayAbove &= replay_fill_samples(prefill) > prefill && replay_fill_samples(prefill) < max_fill_samples(prefill);
    }
    check("prefill options run 10..300 ms in 10 ms steps", stepsOk && PREFILL_OPTIONS == 30);
    check("default option is 50 ms (2205 samples)",
          prefill_samples(PREFILL_DEFAULT_OPTION) == 2205 && samples_to_ms(2205) == 50);
    check("overrun threshold is prefill + 250 ms and fits the ring", fits && max_fill_samples(2205) == 2205 + 11025);
    check("replay fill sits between prefill and overrun threshold", replayAbove && replay_fill_samples(2205) == 4410);

    check("meter rises instantly", near(meter_next_db(-40.0f, -10.0f, 20), -10.0f));
    check("meter falls 20 dB per second", near(meter_next_db(-10.0f, -60.0f, 500), -20.0f));
    float stepped = -10.0f;
    for (int i = 0; i < 50; i++) stepped = meter_next_db(stepped, -60.0f, 20);
    check("meter fall is the same in small steps", near(stepped, -30.0f));
    check("meter falls from a lower level than the previous bar", near(meter_next_db(-10.0f, -15.0f, 20), -10.4f));
    check("meter stops at the floor", near(meter_next_db(-59.0f, -60.0f, 1000), METER_FLOOR_DB));
    check("meter holds with no time elapsed", near(meter_next_db(-30.0f, -60.0f, 0), -30.0f));
    return failed;
}
