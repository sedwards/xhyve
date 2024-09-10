#include "MySwiftWrapper.h"
#include <stdio.h>

int main() {
    int cpuCount = computeCPUCount();
    uint64_t memorySize = computeMemorySize();
    
    printf("CPU Count: %d\n", cpuCount);
    printf("Memory Size: %llu bytes\n", memorySize);

    return 0;
}

