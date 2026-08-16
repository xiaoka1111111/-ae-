#include <stdio.h>
#include "AEConfig.h"
#include "AE_Effect.h"
int main() {
    printf("2.1 = %lu\n", (unsigned long)PF_VERSION(2,1,0,PF_Stage_DEVELOP,1));
    printf("1.4 = %lu (verify vs 655361)\n", (unsigned long)PF_VERSION(1,4,0,PF_Stage_DEVELOP,1));
    return 0;
}
