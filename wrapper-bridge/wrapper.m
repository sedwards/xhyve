#import "MySwiftWrapper.h"
#import "MyProject-Swift.h" // This is the auto-generated header

@implementation MySwiftWrapper

+ (int)computeCPUCount {
    return (int)[MySwiftClass computeCPUCount];
}

+ (uint64_t)computeMemorySize {
    return [MySwiftClass computeMemorySize];
}

@end

