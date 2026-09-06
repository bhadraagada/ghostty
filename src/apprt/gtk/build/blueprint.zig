//! Compiles a blueprint file using `blueprint-compiler`. This performs
//! additional checks to ensure that various minimum versions are met.
//!
//! Usage: blueprint.zig <major> <minor> <output> <input>
//!
//! Example: blueprint.zig 1 5 output.ui input.blp

const std = @import("std");
const builtin = @import("builtin");

pub const c = @cImport({
    @cInclude("adwaita.h");
});

pub const blueprint_compiler_help =
    \\
    \\When building from a Git checkout, Ghostty requires
    \\version {f} or newer of `blueprint-compiler` as a
    \\build-time dependency. Please install it, ensure that it
    \\is available on your PATH, and then retry building Ghostty.
    \\See `HACKING.md` for more details.
    \\
    \\This message should *not* appear for normal users, who
    \\should build Ghostty from official release tarballs instead.
    \\Please consult https://ghostty.org/docs/install/build for
    \\more information on the recommended build instructions.
;

const adwaita_version = std.SemanticVersion{
    .major = c.ADW_MAJOR_VERSION,
    .minor = c.ADW_MINOR_VERSION,
    .patch = c.ADW_MICRO_VERSION,
};
const required_blueprint_version = std.SemanticVersion{
    .major = 0,
    .minor = 16,
    .patch = 0,
};

pub fn main() !void {
    var debug_allocator: std.heap.DebugAllocator(.{}) = .init;
    defer _ = debug_allocator.deinit();
    const alloc = debug_allocator.allocator();

    // Get our args
    var it = try std.process.argsWithAllocator(alloc);
    defer it.deinit();
    _ = it.next(); // Skip argv0
    const arg_major = it.next() orelse return error.NoMajorVersion;
    const arg_minor = it.next() orelse return error.NoMinorVersion;
    const output = it.next() orelse return error.NoOutput;
    const input = it.next() orelse return error.NoInput;

    const required_adwaita_version = std.SemanticVersion{
        .major = try std.fmt.parseUnsigned(u8, arg_major, 10),
        .minor = try std.fmt.parseUnsigned(u8, arg_minor, 10),
        .patch = 0,
    };
    if (adwaita_version.order(required_adwaita_version) == .lt) {
        std.debug.print(
            \\`libadwaita` is too old.
            \\
            \\Ghostty requires a version {f} or newer of `libadwaita` to
            \\compile this blueprint. Please install it, ensure that it is
            \\available on your PATH, and then retry building Ghostty.
        , .{required_adwaita_version});
        std.posix.exit(1);
    }

    // Version checks
    {
        var stdout: std.ArrayListUnmanaged(u8) = .empty;
        defer stdout.deinit(alloc);
        var stderr: std.ArrayListUnmanaged(u8) = .empty;
        defer stderr.deinit(alloc);

        // On Windows, blueprint-compiler is a Python script without .exe extension,
        // so we need to invoke it through the Python interpreter with the script path
        const blueprint_args = if (builtin.os.tag == .windows)
            &[_][]const u8{ "python3.exe", "C:/msys64/ucrt64/bin/blueprint-compiler", "--version" }
        else
            &[_][]const u8{ "blueprint-compiler", "--version" };

        var blueprint_compiler = std.process.Child.init(
            blueprint_args,
            alloc,
        );
        blueprint_compiler.stdout_behavior = .Pipe;
        blueprint_compiler.stderr_behavior = .Pipe;
        // On Windows, ensure Python uses UTF-8 encoding
        var env_map: ?std.process.EnvMap = null;
        if (builtin.os.tag == .windows) {
            env_map = try std.process.getEnvMap(alloc);
            try env_map.?.put("PYTHONUTF8", "1");
            blueprint_compiler.env_map = &env_map.?;
        }
        defer if (env_map) |*em| em.deinit();
        try blueprint_compiler.spawn();
        try blueprint_compiler.collectOutput(
            alloc,
            &stdout,
            &stderr,
            std.math.maxInt(u16),
        );
        const term = blueprint_compiler.wait() catch |err| switch (err) {
            error.FileNotFound => {
                std.debug.print(
                    \\`blueprint-compiler` not found.
                ++ blueprint_compiler_help,
                    .{required_blueprint_version},
                );
                std.posix.exit(1);
            },
            else => return err,
        };
        switch (term) {
            .Exited => |rc| if (rc != 0) std.process.exit(1),
            else => std.process.exit(1),
        }

        const version = try std.SemanticVersion.parse(std.mem.trim(
            u8,
            stdout.items,
            &std.ascii.whitespace,
        ));
        if (version.order(required_blueprint_version) == .lt) {
            std.debug.print(
                \\`blueprint-compiler` is the wrong version.
            ++ blueprint_compiler_help,
                .{required_blueprint_version},
            );
            std.posix.exit(1);
        }
    }

    // Compilation
    {
        var stdout: std.ArrayListUnmanaged(u8) = .empty;
        defer stdout.deinit(alloc);
        var stderr: std.ArrayListUnmanaged(u8) = .empty;
        defer stderr.deinit(alloc);

        // On Windows, blueprint-compiler is a Python script without .exe extension,
        // so we need to invoke it through the Python interpreter with the script path
        const compile_args = if (builtin.os.tag == .windows)
            &[_][]const u8{ "python3.exe", "C:/msys64/ucrt64/bin/blueprint-compiler", "compile", "--output", output, input }
        else
            &[_][]const u8{ "blueprint-compiler", "compile", "--output", output, input };

        var blueprint_compiler = std.process.Child.init(
            compile_args,
            alloc,
        );
        blueprint_compiler.stdout_behavior = .Pipe;
        blueprint_compiler.stderr_behavior = .Pipe;
        // On Windows, ensure Python uses UTF-8 encoding
        var compile_env_map: ?std.process.EnvMap = null;
        if (builtin.os.tag == .windows) {
            compile_env_map = try std.process.getEnvMap(alloc);
            try compile_env_map.?.put("PYTHONUTF8", "1");
            blueprint_compiler.env_map = &compile_env_map.?;
        }
        defer if (compile_env_map) |*em| em.deinit();
        try blueprint_compiler.spawn();
        try blueprint_compiler.collectOutput(
            alloc,
            &stdout,
            &stderr,
            std.math.maxInt(u16),
        );
        const term = blueprint_compiler.wait() catch |err| switch (err) {
            error.FileNotFound => {
                std.debug.print(
                    \\`blueprint-compiler` not found.
                ++ blueprint_compiler_help,
                    .{required_blueprint_version},
                );
                std.posix.exit(1);
            },
            else => return err,
        };

        switch (term) {
            .Exited => |rc| {
                if (rc != 0) {
                    std.debug.print("{s}", .{stderr.items});
                    std.process.exit(1);
                }
            },
            else => {
                std.debug.print("{s}", .{stderr.items});
                std.process.exit(1);
            },
        }
    }
}
