Startup runs exactly once, even though the test scenario starts three shells in separate sessions. The shell documentation needs updating to reflect this.

A quick smoke test of the actual distribution image was followed by the final full test runs.

Session support has been implemented and verified. All existing scenarios and three new ones pass under every tick schedule, in both builds (with and without `-fcopy-params`): 117 runs per build. The `u16.zip` archive contains the updated source tree without build artifacts.

## User-visible behavior

- **Keyboard shortcuts:** Ctrl+] switches to the next session, wrapping around; Ctrl+[ switches to the previous one. These shortcuts work in any console mode, including `NOBREAK`, and are not delivered to applications.
- **Session listing:**
  ```
  0:/data> session
    0    80K  0:/sys/shell.cx > 0:/bin/cat.cx
  * 2    56K  0:/sys/shell.cx
  ```
- **The `session` command:**
  - `session N` switches to session N.
  - `session new [-b] [command ...]` creates a new session running a shell or a program in the current directory. With `-b`, the current session is not switched.
  - `session kill [-f] [N]` terminates a session. Without `-f`, a session with more than one running process cannot be terminated and reports `use kill -f`.
- **Session termination:** When the session's root process exits, the session closes and the next session becomes active. Exiting the last session behaves as before: exit code 0 halts the machine; any other code restarts the shell.
- **System calls:** `SESS_INFO $70`, `SESS_NEW $71`, `SESS_SWITCH $72`, and `SESS_KILL $73`. `SESS_INFO` returns information on each process in the session's process chain, including its state, memory allocation, and command line.

## Important details

- **Ctrl+[ in the terminal:** In the command-line emulator, Ctrl+[ is indistinguishable from Esc, so only Ctrl+] works there. Cycling forward is sufficient with two or three sessions.
- **Web debugger update required:** The web debugger is not included in this archive and needs SCRD register support for the kernel. Without it, the kernel saves blank screens. Implement the same behavior as in `cpu16_machine.hpp`:
  - A word read from `$FF50` returns `character | (attribute << 8)` and advances the position, as in coordinate mode.
  - Byte reads from `$FF50`/`$FF51` return the character and attribute respectively, without advancing the position.
- **The resident area was almost full:** Only 20 bytes were free, rather than the approximately 134 bytes stated in the documentation. Therefore `sys_table` was moved into kernel code; the resident area is now occupied through `$E394`.
- **Kernel memory headroom:**
  - Kernel code occupies 27.0 KB. The documentation recommends considering overlays once it reaches 28 KB.
  - Kernel data occupies 7.7 KB of the available 8 KB.
  - Screens, console input lines, and process names are kept in a separate memory frame for each session.
- **Pending input is discarded on a switch:** Keyboard events received before the switch completes belong to the previous session and are lost.
- **Startup runs only once:** A shell created in a new session does not check volumes or execute `0:/sys/startup`; these actions take place only when the system starts.
- **Changed `fm` test expectation:** Free space on the volume has decreased from 916K to 914K because `shell.cx` has grown.

## Verification

- **`sess`:** All system calls, including error cases, and the following behaviors:
  - Switching away from a compute loop while verifying condition flags between `CMP` and `JNZ`, plus registers R4–R12 and LR.
  - Resumption of `CON_GETEV`.
  - `SLEEP` sleeping only for the remaining interval.
  - Restoring screen contents and display mode.
  - Killing the current session.
  - Continued operation after session 0 closes.
- **`session`:** The shell command.
- **`sessfm`:** The file manager in one session and a shell in another; after switching back, the screen snapshot shows the entire file-manager display.

Documentation has been updated: *Machine* to Revision 9, *System Calls* to Revision 5, *Kernel Architecture* to Revision 5, and *Shell* to Revision 5. The copies in Project are outdated and should be replaced.
