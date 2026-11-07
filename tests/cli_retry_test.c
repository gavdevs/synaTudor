/*
 * Offline checks of the CLI's bounded capture-retry policy and of the verify
 * finger selection helper.
 *
 * The CLI must retry a rejected capture (bad placement) but stop after a fixed
 * number of rejections instead of spinning forever when the engine rejects every
 * sample (e.g. no usable enrolled template).
 */
#include <assert.h>
#include <stdio.h>
#include "cli.h"

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    assert(CLI_MAX_CAPTURE_RETRIES > 0);

    int retries = 0, allowed = 0;
    while(cli_capture_retry_pending(&retries)) {
        allowed++;
        assert(allowed <= CLI_MAX_CAPTURE_RETRIES);
    }

    assert(allowed == CLI_MAX_CAPTURE_RETRIES);
    assert(retries == CLI_MAX_CAPTURE_RETRIES);

    //Verify finger selection: 0 keeps the any-finger behaviour, 1-10 map to the
    //corresponding finger, anything out of range falls back to any.
    assert(cli_parse_finger(0) == TUDOR_FINGER_ANY);
    assert(cli_parse_finger(1) == TUDOR_FINGER_RH_THUMB);
    assert(cli_parse_finger(2) == TUDOR_FINGER_RH_INDEX_FINGER);
    assert(cli_parse_finger(5) == TUDOR_FINGER_RH_LITTLE_FINGER);
    assert(cli_parse_finger(6) == TUDOR_FINGER_LH_THUMB);
    assert(cli_parse_finger(10) == TUDOR_FINGER_LH_LITTLE_FINGER);
    assert(cli_parse_finger(11) == TUDOR_FINGER_ANY);
    assert(cli_parse_finger(0xffffffff) == TUDOR_FINGER_ANY);

    //Once the budget is spent the helper keeps refusing and doesn't count further
    assert(cli_capture_retry_pending(&retries) == false);
    assert(retries == CLI_MAX_CAPTURE_RETRIES);

    puts("[TEST] CLI capture retry bound checks passed");
    return 0;
}
