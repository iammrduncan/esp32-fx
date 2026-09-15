// SPDX-License-Identifier: MIT
const std = @import("std");
const allocator = @import("compact-allocator.zig").allocator;

pub fn main() !void {
    try std.heap.testAllocator(allocator);
    try std.heap.testAllocatorAligned(allocator);
    try std.heap.testAllocatorLargeAlignment(allocator);
    try std.heap.testAllocatorAlignedShrink(allocator);
    // Repeatedly reuse differently sized blocks instead of growing per turn.
    for (0..30) |_| {
        const small = try allocator.alloc(u8, 32769);
        const big = try allocator.alloc(u8, 90001);
        @memset(small, 0x5a);
        @memset(big, 0xa5);
        allocator.free(small);
        allocator.free(big);
    }
    std.debug.print("COMPACT_ALLOCATOR_PASS\n", .{});
}
