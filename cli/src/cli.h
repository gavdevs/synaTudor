#ifndef CLI_CLI_H
#define CLI_CLI_H

#include <tudor/tudor.h>

//Number of capture retries the CLI accepts before giving up on a verify/identify
//request. A rejected sample may be retried, but an engine that rejects every
//capture (for example because it has no usable enrolled template) must not loop.
#define CLI_MAX_CAPTURE_RETRIES 5

//Counts one rejected capture. Returns true while another capture attempt is allowed.
bool cli_capture_retry_pending(int *retries);

//Maps the finger index entered at the verify prompt to a biometric subfactor.
//0 (or an out-of-range value) selects any finger, 1-10 selects that finger.
enum tudor_finger cli_parse_finger(unsigned value);

void cli_main_loop(struct tudor_device *device);

#endif