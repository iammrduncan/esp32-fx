// SPDX-License-Identifier: MIT
// A single-threaded arena for the embedded fx turn lifetime.
//
// std.heap.ArenaAllocator deliberately grows new nodes from the size of the
// previous node. That is a good throughput tradeoff on desktop systems, but a
// long tool turn can retain only a few bytes and still reserve another large
// node. This arena uses fixed 512-byte nodes for small allocations and exact-size
// nodes for larger allocations.
const std = @import("std");

const Allocator = std.mem.Allocator;
const chunk_bytes: usize = 512;

pub const CompactArenaAllocator = struct {
    child_allocator: Allocator,
    head: ?*Node = null,

    const Node = struct {
        next: ?*Node,
        total_len: usize,
        used: usize,
        last_start: usize,
        last_len: usize,
    };

    pub fn init(child_allocator: Allocator) CompactArenaAllocator {
        return .{ .child_allocator = child_allocator };
    }

    pub fn deinit(self: CompactArenaAllocator) void {
        var current = self.head;
        while (current) |node| {
            const next = node.next;
            const bytes = @as([*]u8, @ptrCast(node))[0..node.total_len];
            self.child_allocator.rawFree(bytes, .of(Node), @returnAddress());
            current = next;
        }
    }

    pub fn allocator(self: *CompactArenaAllocator) Allocator {
        return .{
            .ptr = self,
            .vtable = &.{
                .alloc = alloc,
                .resize = resize,
                .remap = remap,
                .free = free,
            },
        };
    }

    pub fn reservedBytes(self: CompactArenaAllocator) usize {
        var total: usize = 0;
        var current = self.head;
        while (current) |node| : (current = node.next) total += node.total_len;
        return total;
    }

    fn data(node: *Node) []u8 {
        return @as([*]u8, @ptrCast(node))[@sizeOf(Node)..node.total_len];
    }

    fn allocateFrom(node: *Node, len: usize, alignment: std.mem.Alignment) ?[*]u8 {
        const bytes = data(node);
        const aligned = std.mem.alignForward(
            usize,
            @intFromPtr(bytes.ptr) + node.used,
            alignment.toByteUnits(),
        ) - @intFromPtr(bytes.ptr);
        if (aligned > bytes.len or len > bytes.len - aligned) return null;
        node.used = aligned + len;
        node.last_start = aligned;
        node.last_len = len;
        return bytes[aligned..][0..len].ptr;
    }

    fn alloc(raw: *anyopaque, len: usize, alignment: std.mem.Alignment, ret_addr: usize) ?[*]u8 {
        const self: *CompactArenaAllocator = @ptrCast(@alignCast(raw));
        if (self.head) |node| if (allocateFrom(node, len, alignment)) |result| return result;

        const padding = alignment.toByteUnits() - 1;
        const minimum = std.math.add(usize, @sizeOf(Node), padding) catch return null;
        const required = std.math.add(usize, minimum, len) catch return null;
        const total_len = @max(required, @sizeOf(Node) + chunk_bytes);
        const bytes = self.child_allocator.rawAlloc(total_len, .of(Node), ret_addr) orelse return null;
        const node: *Node = @ptrCast(@alignCast(bytes));
        node.* = .{
            .next = self.head,
            .total_len = total_len,
            .used = 0,
            .last_start = 0,
            .last_len = 0,
        };
        self.head = node;
        return allocateFrom(node, len, alignment);
    }

    fn resize(raw: *anyopaque, memory: []u8, _: std.mem.Alignment, new_len: usize, _: usize) bool {
        const self: *CompactArenaAllocator = @ptrCast(@alignCast(raw));
        if (new_len <= memory.len) {
            const node = self.head orelse return true;
            const bytes = data(node);
            if (memory.ptr == bytes.ptr + node.last_start and memory.len == node.last_len) {
                node.used = node.last_start + new_len;
                node.last_len = new_len;
            }
            return true;
        }
        const node = self.head orelse return false;
        const bytes = data(node);
        if (memory.ptr != bytes.ptr + node.last_start or memory.len != node.last_len) return false;
        if (node.last_start > bytes.len or new_len > bytes.len - node.last_start) return false;
        node.used = node.last_start + new_len;
        node.last_len = new_len;
        return true;
    }

    fn remap(raw: *anyopaque, memory: []u8, alignment: std.mem.Alignment, new_len: usize, ret_addr: usize) ?[*]u8 {
        if (resize(raw, memory, alignment, new_len, ret_addr)) return memory.ptr;
        const result = alloc(raw, new_len, alignment, ret_addr) orelse return null;
        const copy_len = @min(memory.len, new_len);
        @memcpy(result[0..copy_len], memory[0..copy_len]);
        return result;
    }

    fn free(raw: *anyopaque, memory: []u8, _: std.mem.Alignment, _: usize) void {
        const self: *CompactArenaAllocator = @ptrCast(@alignCast(raw));
        const node = self.head orelse return;
        const bytes = data(node);
        if (memory.ptr != bytes.ptr + node.last_start or memory.len != node.last_len) return;
        node.used = node.last_start;
        node.last_len = 0;
    }
};

test "compact arena retains values and honors alignment" {
    var arena = CompactArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    const first = try alloc.alloc(u8, 100);
    @memset(first, 0xa5);
    const aligned = try alloc.alignedAlloc(u64, .@"64", 600);
    @memset(aligned, 0x1122334455667788);
    for (first) |byte| try std.testing.expectEqual(@as(u8, 0xa5), byte);
    try std.testing.expectEqual(@as(usize, 0), @intFromPtr(aligned.ptr) % 64);
}

test "compact arena bounds tiny-allocation reservation and reclaims last resize" {
    var arena = CompactArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var allocations: [20][]u8 = undefined;
    for (&allocations) |*allocation| allocation.* = try alloc.alloc(u8, 100);
    try std.testing.expect(arena.reservedBytes() <= 3 * 1024);

    allocations[19] = try alloc.realloc(allocations[19], 25);
    const reused = try alloc.alloc(u8, 75);
    try std.testing.expectEqual(allocations[19].ptr + 25, reused.ptr);
}
