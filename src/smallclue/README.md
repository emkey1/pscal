# smallclue

**smallclue** is a lightweight, multicall binary that provides a suite of standard Unix-like utilities. It is designed specifically for constrained or sandboxed environments where standard GNU/BSD core utilities are unavailable, such as custom terminal emulators on iOS and iPadOS (e.g., PSCAL).

Functionally similar to BusyBox, `smallclue` combines many common tools (like `ls`, `cp`, `grep`, `ssh`) into a single executable to reduce overhead and simplify integration.

## Overview

* **Multicall Architecture:** Invoking the binary as `smallclue ls` or symlinking `ls` to `smallclue` runs the `ls` applet.
* **Zero-Dependency Implementations:** Most core utilities are implemented directly in C within `src/core.c` to minimize external dependencies.
* **Third-Party Integration:** Includes wrappers for complex tools like **OpenSSH** and **Nextvi**.
* **iOS Specifics:** Features applets designed for iOS quirks, such as `pbcopy`/`pbpaste` for system clipboard access and specialized path virtualization hooks.

## Available Applets

`smallclue` currently implements the following commands:

### File Management
* **ls**: List directory contents (supports colors, `-l`, `-h`, `-a`, `-t`).
* **cp**: Copy files and directories (recursive).
* **mv**: Move or rename files.
* **rm**: Remove files and directories.
* **mkdir** / **rmdir**: Create or remove directories.
* **touch**: Update file timestamps or create empty files.
* **ln**: Create links (symbolic and hard).
* **pwd**: Print working directory.
* **chmod**: Change file modes/permissions (supports octal and symbolic `u+x`).
* **du**: Estimate file space usage.
* **df**: Report file system disk space usage.
* **file**: Determine file type.
* **basename** / **dirname**: Parse path components.

### Text Processing & Filtering
* **cat**: Concatenate and print files.
* **grep**: File pattern searcher (supports `-i`, `-v`, `-n`).
* **head** / **tail**: Output the first/last part of files (tail supports `-f` follow).
* **more** / **less**: File paging filters.
* **wc**: Word, line, character, and byte count.
* **sort**: Sort lines of text files.
* **uniq**: Report or omit repeated lines.
* **cut**: Remove sections from each line of files.
* **sed**: Stream editor (basic substitution support).
* **tr**: Translate or delete characters.
* **tee**: Read from standard input and write to standard output and files.

### Editors & Viewers
* **vi** / **nextvi**: A small, efficient vi-like text editor.
* **md**: A terminal-based Markdown viewer (renders tables, headers, and lists interactively).

### Networking
* **ssh**: OpenSSH client wrapper.
* **scp**: Secure copy (OpenSSH).
* **sftp**: Secure file transfer (OpenSSH).
* **ssh-keygen**: Generate authentication keys.
* **ping**: TCP-based ping utility.
* **curl** / **wget**: Tools for transferring data with URL syntax (wrappers).
* **telnet**: Simple Telnet client.
* **nslookup** / **host**: DNS lookup utilities.
* **traceroute**: Trace the route packets take to a network host.
* **ipaddr**: Display network interface addresses.

### Shell & System
* **sh**: Launches the PSCAL shell frontend (`exsh`).
* **env**: Run a program in a modified environment.
* **ps**: Report a snapshot of current processes.
* **kill**: Send signals to processes.
* **uptime**: Tell how long the system has been running.
* **date**: Print or set the system date and time.
* **cal**: Display a calendar.
* **clear** / **cls**: Clear the terminal screen.
* **sleep**: Delay for a specified amount of time.
* **watch**: Execute a program periodically, showing output fullscreen.
* **xargs**: Build and execute command lines from standard input.
* **test** / **[**: Evaluate conditional expressions.
* **true** / **false**: Return success or failure status.

### iOS / Runtime Utilities
* **pbcopy**: Copy data from stdin to the iOS system clipboard.
* **pbpaste**: Paste data from the iOS system clipboard to stdout.
* **dmesg**: Prints the PSCAL runtime session log.
* **licenses**: View open source licenses included in the distribution.
* **resize** / **stty**: Terminal setting manipulation.

## Usage

Can be run directly via the main entry point if built as a standalone executable:

```bash
./smallclue <command> [arguments...]
