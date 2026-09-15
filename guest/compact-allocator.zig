// SPDX-License-Identifier: MIT
// Explicit ABI names avoid Zig libc's internally bound allocation functions.
const std = @import("std");
extern "c" fn fx_malloc(usize) ?[*]u8;
extern "c" fn fx_free([*]u8) void;
extern "c" fn fx_realloc([*]u8, usize) ?[*]u8;
extern "c" fn fx_usable_size([*]u8) usize;

pub const allocator: std.mem.Allocator = .{
    .ptr = undefined,
    .vtable = &.{ .alloc = alloc, .resize = resize, .remap = remap, .free = free },
};

fn alloc(_: *anyopaque, len: usize, alignment: std.mem.Alignment, _: usize) ?[*]u8 {
    const a = @max(alignment.toByteUnits(), @alignOf(usize));
    const total = std.math.add(usize, len, a + @sizeOf(usize)) catch return null;
    const base = fx_malloc(total) orelse return null;
    const address = std.mem.alignForward(usize, @intFromPtr(base) + @sizeOf(usize), a);
    const header: *usize = @ptrFromInt(address - @sizeOf(usize));
    header.* = @intFromPtr(base);
    return @ptrFromInt(address);
}

fn resize(_: *anyopaque, memory: []u8, _: std.mem.Alignment, new_len: usize, _: usize) bool {
    const address = @intFromPtr(memory.ptr);
    const header: *const usize = @ptrFromInt(address - @sizeOf(usize));
    return new_len <= fx_usable_size(@ptrFromInt(header.*)) - (address - header.*);
}
fn remap(_: *anyopaque, memory: []u8, alignment: std.mem.Alignment, new_len: usize, _: usize) ?[*]u8 {
    const a = @max(alignment.toByteUnits(), @alignOf(usize));
    const total = std.math.add(usize, new_len, a + @sizeOf(usize)) catch return null;
    const old_address = @intFromPtr(memory.ptr);
    const old_header: *const usize = @ptrFromInt(old_address - @sizeOf(usize));
    const offset = old_address - old_header.*;
    const base = fx_realloc(@ptrFromInt(old_header.*), total) orelse return null;
    const address = std.mem.alignForward(usize, @intFromPtr(base) + @sizeOf(usize), a);
    const result: [*]u8 = @ptrFromInt(address);
    const count = @min(memory.len, new_len);
    // realloc preserves bytes at the old offset, but alignment padding can change.
    const old_data = base[offset..][0..count];
    if (address <= @intFromPtr(old_data.ptr))
        std.mem.copyForwards(u8, result[0..count], old_data)
    else
        std.mem.copyBackwards(u8, result[0..count], old_data);
    const header: *usize = @ptrFromInt(address - @sizeOf(usize));
    header.* = @intFromPtr(base);
    return result;
}
fn free(_: *anyopaque, memory: []u8, _: std.mem.Alignment, _: usize) void {
    const header: *const usize = @ptrFromInt(@intFromPtr(memory.ptr) - @sizeOf(usize));
    fx_free(@ptrFromInt(header.*));
}
