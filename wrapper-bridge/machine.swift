import Foundation

@objc public class MySwiftClass: NSObject {
    @objc public static func computeCPUCount() -> Int {
        let totalAvailableCPUs = ProcessInfo.processInfo.processorCount
        var virtualCPUCount = totalAvailableCPUs <= 1 ? 1 : totalAvailableCPUs - 1
        virtualCPUCount = max(virtualCPUCount, 1)
        virtualCPUCount = min(virtualCPUCount, 32)
        return virtualCPUCount
    }
    
    @objc public static func computeMemorySize() -> UInt64 {
        var memorySize: UInt64 = 4 * 1024 * 1024 * 1024 // 4 GB
        memorySize = max(memorySize, 1 * 1024 * 1024 * 1024) // Min 1 GB
        memorySize = min(memorySize, 64 * 1024 * 1024 * 1024) // Max 64 GB
        return memorySize
    }
}

