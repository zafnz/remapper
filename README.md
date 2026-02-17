# remapper

Redirect filesystem paths for any program on macOS. Run multiple instances of the same application, each with its own isolated configuration directory. **Does not use symlinks** so programs won't clobber each other. This works by catching the programs read(), write(), etc. calls and redirecting them. 

```bash
alias claude-personal='remapper ~/claude-personal "~/.claude*" claude'
alias claude-work='remapper ~/claude-work "~/.claude*" claude'
```

Now `claude-personal` and `claude-work` each get their own separate `~/.claude/` directory, completely independent of each other and the default.

## Install

```bash
mkdir -p ~/.local/bin && curl -L -o ~/.local/bin/remapper \
    https://github.com/zafnz/remapper/releases/latest/download/remapper-macos-arm64 \
    && chmod +x ~/.local/bin/remapper
```

Ensure `~/.local/bin` is in your PATH (add to `~/.zshrc` if not already):

```bash
export PATH="$HOME/.local/bin:$PATH"
```

If macOS blocks the binary, run: `xattr -d com.apple.quarantine ~/.local/bin/remapper`

## Usage

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
remapper ~/v1 '~/.claude*' claude

# Multiple mappings (-- required)
remapper ~/isolated '~/.claude*' '~/.config*' -- claude

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

## Environment variables

| Variable | Description | Default |
|---|---|---|
| `RMP_CONFIG` | Base directory for remapper's own config | `~/.remapper/` |
| `RMP_CACHE` | Directory for cached re-signed binaries | `$RMP_CONFIG/cache/` |
| `RMP_DEBUG_LOG` | Log file path (enables debug logging) | unset |

## Malware detection note

This program uses `DYLD_INSERT_LIBRARIES` (similar to `LD_PRELOAD` on Linux), which could set off malware alerts. The program does nothing except change the paths provided -- reads, writes, mkdir, unlink, etc. of the matching path instead go to another path. In order to do this on macOS it needs to make cached copies of programs that have a hardened runtime flag (which causes the OS to ignore `DYLD_INSERT_LIBRARIES`) and re-sign them without that restriction.

The source for this program is quite simple to follow, has no obfuscated code, and should be clear to everyone that it is clean.

If it does set off a malware alarm the author would like to fix that, so please file an issue ticket.

## How it works

remapper has two components:

1. **`remapper`** -- the launcher that sets up the environment and exec's the target program
2. **`interpose.dylib`** -- a dynamic library injected via `DYLD_INSERT_LIBRARIES` that intercepts filesystem calls

When you run `remapper <target-dir> '<mapping>' <program>`:

1. The launcher resolves the mapping patterns and target directory to absolute paths
2. It sets `DYLD_INSERT_LIBRARIES` to load `interpose.dylib` into the target program
3. The dylib intercepts filesystem calls (`open`, `stat`, `mkdir`, `unlink`, `rename`, `readlink`, `opendir`, etc.) and rewrites any path that matches a mapping pattern so it points into the target directory instead

For example, with `remapper ~/work '~/.claude*' claude`:

- A call to `open("/Users/you/.claude/config")` becomes `open("/Users/you/work/.claude/config")`
- A call to `mkdir("/Users/you/.claude")` becomes `mkdir("/Users/you/work/.claude")`
- Paths that don't match the pattern are left untouched

### Handling hardened binaries

macOS binaries signed with hardened runtime silently strip `DYLD_INSERT_LIBRARIES`. remapper detects this and automatically:

1. Copies the binary to a cache directory (`~/.remapper/cache/`)
2. Re-signs it with ad-hoc signature and an entitlement that allows `DYLD_INSERT_LIBRARIES`
3. Executes the cached copy instead

This also applies to child processes -- the interposer intercepts `posix_spawn`, `execve`, and friends to ensure the dylib propagates through the entire process tree.

### Handling SIP-protected interpreters

Scripts with shebangs pointing to SIP-protected paths (`/usr/bin/env`, `/bin/sh`, etc.) would normally cause macOS to strip `DYLD_INSERT_LIBRARIES`. remapper detects shebangs and either resolves the interpreter directly (for `#!/usr/bin/env`) or creates a cached re-signed copy of the interpreter.

## Building

```bash
make
```

This produces a single self-contained `build/remapper` binary. The `interpose.dylib` is embedded inside it and automatically extracted to `~/.remapper/interpose.dylib` on first run.


## Intercepted system calls

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

## Running tests

```bash
make test
```

## Requirements

- macOS (uses `DYLD_INSERT_LIBRARIES` and Mach-O codesigning; `codesign` ships with macOS)
- Xcode command line tools (for building from source -- provides `gcc`/`clang`)

## License

This program is copyright 2026, Nick Clifford <nick@nickclifford.com>.

It is distributed under the GNU General Public License v3.0

See [LICENSE](LICENSE) 
