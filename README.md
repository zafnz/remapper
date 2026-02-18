# remapper

Redirect filesystem paths for any program on **macOS** or **Linux** (WSL2 should work too). Run multiple instances of the same application, each with its own isolated configuration directory. **Does not use symlinks** so programs won't clobber each other.

```bash
alias claude-personal='remapper ~/claude-personal "~/.claude*" claude'
alias claude-work='remapper ~/claude-work "~/.claude*" claude'
```
(Note: you need to use quotes around the glob matches `"~/.codex*"` otherwise zsh/bash will interpret it)

Now `claude-personal` and `claude-work` each get their own separate `~/.claude/` directory, completely independent of each other and the default.

## Install

**macOS**
```bash
mkdir -p ~/.local/bin && curl -L -o ~/.local/bin/remapper \
    https://github.com/zafnz/remapper/releases/latest/download/remapper-Darwin-arm64 \
    && chmod +x ~/.local/bin/remapper
```
**Linux (x86_64 / arm64)**
```bash
mkdir -p ~/.local/bin && curl -L -o ~/.local/bin/remapper \
    https://github.com/zafnz/remapper/releases/latest/download/remapper-Linux-x86_64 \
    && chmod +x ~/.local/bin/remapper
```

Ensure `~/.local/bin` is in your PATH (add to `~/.zshrc` or `~/.bashrc` if not already):

```bash
export PATH="$HOME/.local/bin:$PATH"
```

If macOS blocks the binary, run: `xattr -d com.apple.quarantine ~/.local/bin/remapper`

## Usage

**NOTE**: You should run the target program at least once without using remapper so that it establishes the files it needs first. This is particularly important with Linux since it has to map the files at startup.

```
remapper [--debug-log <file>] <target-dir> <mapping>... -- <program> [args...]
```

If there is only one mapping, the `--` separator is optional:

```
remapper <target-dir> <mapping> <program> [args...]
```

### Arguments

- **`<target-dir>`** -- directory where redirected files will live (created if needed)
- **`<mapping>`** -- path pattern to intercept; supports glob wildcards in the last path component (e.g. `~/.claude*` matches `.claude`, `.claude-code`, `.claude.json`, etc.)
- **`<program> [args...]`** -- the command to run with path redirection active

### Examples

```bash
# Single mapping (-- is optional)
remapper ~/claude-personal '~/.claude*' claude

# Multiple mappings (-- required)
remapper ~/isolated '~/.test*' '~/.config*' -- test

# With debug logging
remapper --debug-log /tmp/rmp.log ~/v1 '~/.claude*' claude

# Redirect codex config
remapper ~/codex-alt '~/.codex*' codex --model gpt-4
```

**Important:** Single-quote your mappings to prevent the shell from expanding the glob.

### Multiple mappings

Use `--` to separate mappings from the command when specifying more than one:

```bash
remapper ~/myenv '~/.config/app*' '~/.local/share/app*' -- myapp --flag
```

## FAQs

### The program still says it's using `/the/original/path`!

Yes. As far as the program is concerned it is using the unmapped path, it doesn't know that under the hood everything to `/the/original/path` is going to `/wherever/you/said` -- it has no idea.

### How do I run multiple copies of a GUI app like Codex GUI?

You need to start the program from the terminal:
```bash
remapper ~/.codex-alt '~/.codex*' -- /Applications/Codex.app/Contents/MacOS/Codex
```
(Note: Launching from `codex app` doesn't seem to work)

### Does this work for every program?

On **Linux**, yes -- the redirection happens at the kernel level (mount namespaces), so every program sees the remapped paths regardless of how it's linked or what language it's written in.

On **macOS**, it works for the vast majority of programs. There are edge cases where unusual path construction (e.g. `open("/Users/me/./app.config")` with an embedded `./`) might not be detected. In practice this doesn't happen with config files/dirs.

### Does it make it slower?

On **Linux**, there is zero overhead -- bind mounts are handled by the kernel's VFS layer and are indistinguishable from normal filesystem access.

On **macOS**, the overhead is negligible. The interposer adds a few string comparisons to each filesystem call. We've tested with 100,000 file operations and it's within the noise.

### Why not just use containers?
Good question. For a semi-hostile app that really doesn't want to to be manipulated that would be a good idea.

## Environment variables

| Variable | Description | Default |
|---|---|---|
| `RMP_CONFIG` | Base directory for remapper's own config (macOS only) | `~/.remapper/` |
| `RMP_CACHE` | Directory for cached re-signed binaries (macOS only) | `$RMP_CONFIG/cache/` |
| `RMP_DEBUG_LOG` | Log file path (enables debug logging) | unset |

## Malware detection note (macOS)

The macOS version uses `DYLD_INSERT_LIBRARIES`, which _could_ set off malware alerts. The program does nothing except change the paths provided -- reads, writes, mkdir, unlink, etc. of the matching path instead go to another path. In order to do this on macOS it needs to make cached copies of programs that have a hardened runtime flag (which causes macOS to ignore `DYLD_INSERT_LIBRARIES`) and re-sign them without that restriction.

The Linux version does not use `LD_PRELOAD` or inject any libraries, so this is not a concern on Linux.

The source for this program is quite simple to follow, has no obfuscated code, and should be clear to everyone that it is clean.

If it does set off a malware alarm the author would like to fix that, so please file an issue ticket.

## How it works

The CLI interface is the same on both platforms, but the underlying mechanism is fundamentally different.

For example, with `remapper ~/work '~/.claude*' claude`:

- A call to `open("/Users/you/.claude/config")` becomes `open("/Users/you/work/.claude/config")`
- A call to `mkdir("/Users/you/.claude")` becomes `mkdir("/Users/you/work/.claude")`
- Paths that don't match the pattern are left untouched

### Linux: mount namespaces

On Linux, remapper uses **kernel mount namespaces** to redirect paths. This works on _every_ binary -- dynamically linked, statically linked (musl/Go), scripts, anything -- because the redirection happens at the kernel's VFS layer, not by intercepting library calls.

When you run `remapper <target-dir> '<mapping>' <program>`:

1. The launcher resolves the mapping patterns and scans the filesystem for matching files and directories (e.g. `~/.claude/`, `~/.claude.json`)
2. It calls `unshare(CLONE_NEWUSER | CLONE_NEWNS)` to create a private **user namespace** and **mount namespace**. This requires no root privileges -- the Linux kernel allows any unprivileged user to create user namespaces
3. For each matching path, it performs a **bind mount**: the target directory's version of the file/directory is mounted over the original path. A bind mount makes content appear at a second location, transparently to all applications
4. It execs the program. The program (and all its children) inherit the mount namespace and see the remapped paths as if they were the originals

Because the mounts exist only within the namespace, they are completely invisible to other processes. When the remapped process exits, the namespace is destroyed and the mounts vanish automatically.

**Note:** Unprivileged user namespaces must be enabled on the system. This is the default on most distributions (Ubuntu, Fedora, Arch, etc.). If not, a system administrator can enable it with `sudo sysctl -w kernel.unprivileged_userns_clone=1`.

**Note:** The program must have been run at least once _without_ remapper so that its config files/directories exist on disk. Remapper scans for existing paths that match the glob patterns -- if nothing exists yet, there's nothing to mount over.

### macOS: DYLD interposition

macOS does not support mount namespaces, so remapper uses a different approach: a dynamic library injected via `DYLD_INSERT_LIBRARIES` that intercepts filesystem calls at the C library level.

remapper has two components on macOS:

1. **`remapper`** -- the launcher that sets up the environment and exec's the target program
2. **`interpose.dylib`** -- a dynamic library embedded inside the remapper binary, injected via `DYLD_INSERT_LIBRARIES`, that intercepts filesystem calls

When you run `remapper <target-dir> '<mapping>' <program>`:

1. The launcher resolves the mapping patterns and target directory to absolute paths
2. It extracts the embedded `interpose.dylib` to `~/.remapper/` (if not already present)
3. It sets `DYLD_INSERT_LIBRARIES` to load the dylib into the target program
4. The dylib intercepts filesystem calls (`open`, `stat`, `mkdir`, `rename`, `execve`, etc.) and rewrites any path that matches a mapping pattern so it points into the target directory instead

#### Handling hardened binaries (macOS)

macOS binaries signed with hardened runtime silently strip `DYLD_INSERT_LIBRARIES`. remapper detects this and automatically:

1. Copies the binary to a cache directory (`~/.remapper/cache/`)
2. Re-signs it with ad-hoc signature and an entitlement that allows `DYLD_INSERT_LIBRARIES`
3. Executes the cached copy instead

This also applies to child processes -- the interposer intercepts `posix_spawn`, `execve`, and friends to ensure the dylib propagates through the entire process tree.

#### Handling SIP-protected interpreters (macOS)

Scripts with shebangs pointing to SIP-protected paths (`/usr/bin/env`, `/bin/sh`, etc.) would normally cause macOS to strip `DYLD_INSERT_LIBRARIES`. remapper detects shebangs and either resolves the interpreter directly (for `#!/usr/bin/env`) or creates a cached re-signed copy of the interpreter.

#### Intercepted system calls (macOS)

The interposer redirects the following filesystem operations:

- `open`, `openat`, `creat`
- `stat`, `lstat`, `fstatat`
- `access`, `faccessat`
- `mkdir`, `mkdirat`
- `unlink`, `unlinkat`
- `rename`, `renameat`
- `rmdir`, `opendir`
- `chdir`
- `readlink`, `readlinkat`
- `chmod`, `fchmodat`
- `chown`, `lchown`, `fchownat`
- `symlink`, `symlinkat`
- `link`, `linkat`
- `truncate`
- `realpath`
- `posix_spawn`, `posix_spawnp`
- `execve`, `execv`, `execvp`

## Building

```bash
make
```

On **macOS**, this produces a single self-contained `build/remapper` binary with the `interpose.dylib` embedded inside it (automatically extracted to `~/.remapper/` on first run).

On **Linux**, this produces a standalone `build/remapper` binary with no external dependencies.

## Running tests

```bash
make test
```

**Note:** On Linux, the tests require unprivileged user namespaces to be enabled. If running in Docker, use `--privileged`.

## Requirements

**macOS**
- Nothing extra to run (`codesign` ships with macOS)
- Building needs Xcode command line tools (provides `gcc`/`clang`)

**Linux**
- Unprivileged user namespaces enabled (default on most distributions)
- Building needs `gcc` and `make`

## License

This program is copyright 2026, Nick Clifford <nick@nickclifford.com>.

It is distributed under the GNU General Public License v3.0

See [LICENSE](LICENSE)
