#include <andueprober/Agent.h>
#include <stdio.h>

int main(void) {
    AUEP_Result result = {0};
    if (AUEP_Query(&result) != AUEP_OK || result.state != AUEP_CREATED) return 1;
    if (AUEP_Initialize(NULL) != AUEP_INVALID_ARGUMENT) return 2;
    if (AUEP_Stop() != AUEP_OK || AUEP_Stop() != AUEP_OK) return 3;
    if (AUEP_Query(&result) != AUEP_OK || result.state != AUEP_STOPPED) return 4;
    puts("PASS: public Agent C API linked through the shared target; inert query, invalid admission and idempotent stop");
    return 0;
}
