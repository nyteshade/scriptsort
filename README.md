# scriptsort

scriptsort sorts and concatenates shell script files from a directory with a
deterministic load order. The output is sourced directly into your shell, giving
you a clean, modular way to manage shell configuration (aliases, functions,
environment setup, path manipulation, etc.) split across as many files as you like.

## Getting started

### 1. Build and install

```sh
./build.sh     # compiles scriptsort and ms to .local/bin/
./install.sh   # copies binaries to $HOME/.local/bin, patches .zshrc / .bashrc
```

`ms` is an optional companion binary that prints milliseconds since epoch, used
by `--debug` for startup timing. Install it alongside `scriptsort` in `$PATH` to
enable that feature.

### 2. Set up your scripts directory

The recommended layout separates shell-agnostic scripts from shell-specific ones:

```
$HOME/.local/scripts/
  shared/               # sourced in every shell
    ordered.0.env
    ordered.1.path
    aliases
    functions
  bash/                 # sourced only in bash
    completions
  zsh/                  # sourced only in zsh
    completions
    ordered.0.opts
```

`shared/`, `bash/`, and `zsh/` are the expected subdirectory names. Only the
directories that exist are used — you can start with just `shared/`.

### 3. Add one line to each shell config

In both `.zshrc` and `.bashrc`:

```sh
source <(scriptsort bundle -s $HOME/.local/scripts)
```

That's it. scriptsort detects which shell is running, bundles `shared/` first,
then adds the appropriate shell-specific subdirectory. The same command works in
both config files.

---

## File naming convention

Files in a managed directory are sorted into three groups, in this order:

| Pattern | Position | Sorted by |
|---|---|---|
| `ordered.0` – `ordered.49` | First | Numeric, ascending |
| Everything else | Middle | Alphabetical |
| `ordered.50` – `ordered.N` | Last | Numeric, ascending |

The cutoff between "first" and "last" groups defaults to 50 and can be changed
with `--cutoff`.

**Example load order:**

```
ordered.0.env          ← first group, numeric
ordered.1.path
aliases                ← middle group, alphabetical
functions
prompt
ordered.50.cleanup     ← last group, numeric
ordered.999.post-init
```

**Rules:**
- Files prefixed with `skip.` are silently ignored by all subcommands.
- When two ordered files share the same number, they sort alphabetically by
  the part of the filename after `ordered.<n>.` — so `ordered.5.aaa` comes
  before `ordered.5.zzz`.

---

## Subcommands

### `bundle`

The primary subcommand. Concatenates script file contents in sorted order and
wraps the result in an ERR trap that reports the originating file and line number
if anything fails. Source the output via process substitution:

```sh
source <(scriptsort bundle <directory>)
source <(scriptsort bundle -s <base-dir>)
```

#### Single-directory form

Points at one directory and bundles everything in it:

```sh
source <(scriptsort bundle $HOME/.local/scripts/shared)
```

This is the right choice if you don't want automatic shell-specific separation —
you manage which directories to source yourself. Before `-s` existed, the typical
setup looked like this:

**.zshrc**
```sh
source <(scriptsort bundle $HOME/.local/scripts/shared)
source <(scriptsort bundle $HOME/.local/scripts/zsh)
```

**.bashrc**
```sh
source <(scriptsort bundle $HOME/.local/scripts/shared)
source <(scriptsort bundle $HOME/.local/scripts/bash)
```

Each config file needed two lines: one for shared scripts, one for the
shell-specific directory.

#### `--scripts-dir` / `-s` form

Handles the `shared/` + shell subdirectory pattern in a single call, with
automatic shell detection:

```sh
source <(scriptsort bundle -s $HOME/.local/scripts)
```

The two `.zshrc` lines and two `.bashrc` lines above collapse to one line that
works in both files. scriptsort figures out the shell and loads the right
subdirectory.

**Shell detection order:**

1. `--zsh` / `--bash` flag — explicit override, always wins
2. `$ZSH_VERSION` — exported by zsh at startup
3. `$BASH_VERSION` — exported by bash at startup
4. Basename of `$SHELL` — e.g. `/bin/zsh` → `zsh`

Detection is reliable in the common case, but environment variables are not
always present in non-login or non-interactive shells, and some setups source
config files in contexts where these variables haven't been set yet. If you'd
rather not depend on detection, pin the shell explicitly in each config file:

```sh
# .zshrc — guaranteed, no detection required
source <(scriptsort bundle -s $HOME/.local/scripts --zsh)

# .bashrc — guaranteed, no detection required
source <(scriptsort bundle -s $HOME/.local/scripts --bash)
```

If detection fails and no override is given, `shared/` is still bundled — the
shell-specific subdirectory is simply skipped without error.

**Options:**

| Flag | Description |
|---|---|
| `-s, --scripts-dir <dir>` | Bundle `shared/` then the detected shell subdirectory |
| `--zsh` | Use `zsh/` subdirectory; bypasses detection (requires `-s`) |
| `--bash` | Use `bash/` subdirectory; bypasses detection (requires `-s`) |
| `--debug` | Wrap bundle with timing; exports `SCRIPTSORT_ELAPSED` |
| `--cutoff <n>` | Change the ordered/unordered boundary (default: 50) |

---

### `list`

Prints filenames in sorted load order, one per line. Useful for verifying order
before committing to it.

```sh
scriptsort list <directory> [--cutoff <n>]
```

---

### `init`

Emits a self-contained shell function (`includeScripts`) that sources each file
individually by path rather than concatenating them. An alternative to `bundle`
when you prefer per-file sourcing.

```sh
scriptsort init <directory> [--debug] [--cutoff <n>]
```

```sh
source <(scriptsort init "$HOME/.local/scripts/shared")
```

With `--debug`, the generated wrapper records per-file timing using `ms` and
accumulates results in a `timings` array.

---

### `edit`

Creates, appends to, or removes files in a managed scripts directory. Defaults
to the `shared/` subdirectory.

```sh
scriptsort edit [--shared|--bash|--zsh] <command> <file> [text]
```

| Command | Description |
|---|---|
| `write [-f] [-q] <file> [text]` | Create a file; use `-f` to overwrite if it exists |
| `append [-q] <file> [text]` | Append text (with a leading newline) to a file |
| `remove <file>` | Delete a file |

If `text` is omitted, content is read from stdin. `-q` suppresses the filename
echoed on success.

```sh
# Write from a heredoc into zsh/
scriptsort edit --zsh write my-aliases <<'EOF'
alias gs='git status'
alias gd='git diff'
EOF

# Append a line to shared/
scriptsort edit append my-aliases "alias gl='git log --oneline'"

# Remove from bash/
scriptsort edit --bash remove old-stuff
```

---

## Global flags

```sh
scriptsort -v            # print version and exit
scriptsort --version
scriptsort <cmd> --help  # subcommand-specific help
```

---

## Tips and tricks

### Verify load order before you rely on it

`list` is a dry run — it shows exactly what `bundle` would include and in what
order, without reading file contents:

```sh
scriptsort list $HOME/.local/scripts/shared
```

Redirect to a file before and after reordering, then diff to catch surprises.

### Temporarily disable a file without deleting it

Prefix any filename with `skip.` and scriptsort ignores it across all subcommands:

```sh
mv ordered.5.experiments skip.ordered.5.experiments
```

Remove the prefix to re-enable. Works for both ordered and unordered filenames.

### Pin a file to run absolutely last

`ordered.50` and above always load after all unordered files. Use a high number
to ensure a file is always last regardless of what else gets added:

```sh
ordered.999.post-init
```

To push more files into the "always last" group without renaming them, lower the
cutoff instead:

```sh
source <(scriptsort bundle -s $HOME/.local/scripts --cutoff 10)
```

### Profile shell startup with `--debug`

With `ms` in `$PATH`, `--debug` wraps the bundle with timing and exports
`SCRIPTSORT_ELAPSED` in milliseconds:

```sh
source <(scriptsort bundle -s $HOME/.local/scripts --debug)
echo "Scripts loaded in ${SCRIPTSORT_ELAPSED}ms"
```

Add this temporarily to your shell config to measure the cost of your scripts,
then remove it when done.

### Group related files at the same priority

Files with the same order number sort alphabetically by their suffix, so you
can group related scripts at the same tier without breaking ordering between tiers:

```
ordered.10.env-base
ordered.10.env-overrides    ← loads after env-base, before ordered.11+
```

### Use `edit` safely from scripts and automation

`edit write` refuses to overwrite an existing file unless `-f` is passed, making
it safe to call idempotently:

```sh
# Fails loudly if the file already exists — safe to script
scriptsort edit write my-function 'my_func() { echo "hello"; }'

# Intentional overwrite for generated content
scriptsort edit write -f generated-exports "$(some-tool export)"
```

### Cache the bundle for faster startup

Process substitution re-runs scriptsort on every shell start. For large
directories, generate once and source the file:

```sh
scriptsort bundle -s $HOME/.local/scripts > $HOME/.local/scripts/.bundle.sh
source $HOME/.local/scripts/.bundle.sh
```

Regenerate whenever scripts change — wrap it in an alias or a shell function
for convenience.

### Pipe `list` into other tools

`list` output is newline-delimited and composable:

```sh
# Find files missing a shebang line
scriptsort list $HOME/.local/scripts/shared | while read -r f; do
  head -1 "$HOME/.local/scripts/shared/$f" | grep -q '^#!' \
    || echo "No shebang: $f"
done
```
