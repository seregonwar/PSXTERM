You are continuing development of the existing repository:

https://github.com/seregonwar/PSXTERM

This task is STRICTLY INCREMENTAL and intentionally narrow.

Do NOT add new major features.

Do NOT implement:
- challenge-response authentication;
- shell history;
- tab completion;
- pipelines;
- redirection;
- job control;
- Windows client;
- compression;
- new transfer features;
- package manager functionality;
- GUI;
- web UI.

The current goal is:

    STABILIZE CURRENT MAIN
    MAKE CI FULLY GREEN
    FIX CAPABILITY SEMANTICS
    PREPARE HARDWARE TESTING
    STOP

Do not continue beyond that scope.

# Current repository state

Current main already includes:

- PTTY/1;
- psxtermd;
- psh;
- host client;
- diagnostics / doctor;
- PTY and PipeTTY;
- PS4 / PS5 process backends;
- capability negotiation;
- bounded input/output queues;
- persistent sessions;
- detach / attach / resume tokens;
- bounded detached scrollback;
- native push / pull / install;
- host tests;
- sanitizer jobs;
- PS4/PS5 cross-build CI.

Do NOT reimplement these.

# Known current problems

Before making changes, verify these against CURRENT main and CURRENT GitHub Actions logs.

Do not blindly trust this prompt if the repository has already changed.

Known failures from the latest Verify run include:

## 1. macOS host build failure

Current host PTY probe uses:

    TIOCGPTN

directly inside:

    platform/host/tty.c

On current macOS CI this identifier is unavailable.

The host PTY implementation already uses:

    posix_openpt
    grantpt
    unlockpt
    ptsname

successfully.

The diagnostic probe must therefore be portable.

Do NOT pretend Darwin supports Linux/FreeBSD-specific ioctls.

Fix the host probe so that:

- Linux may use TIOCGPTN when available;
- macOS may obtain the slave through ptsname();
- the actual PTY functionality can still be tested;
- the target PS4/PS5 probe remains strict about the FreeBSD mechanisms it actually needs.

Do not weaken the console probe merely to make macOS pass.

Important distinction:

HOST PTY CAPABILITY

is not identical to:

PS4/PS5 FREEBSD PTY CAPABILITY

Do not conflate them.

# TTY probe semantics

The generic result currently contains fields such as:

    ptmx_open
    tiocgptn
    slave_open
    termios
    winsize
    isatty_true
    io_roundtrip

Do not require `tiocgptn == true` for a host POSIX PTY backend on a platform where TIOCGPTN does not exist.

Instead, make backend availability explicitly platform-aware.

Possible approach:

    result->slave_discovery

or another clear representation.

For example:

    TIOCGPTN
    PTSNAME
    PLATFORM_NATIVE

Do not add complexity merely for aesthetics, but avoid using one Linux/FreeBSD-specific boolean as a universal definition of PTY support.

Acceptance criteria:

- Linux host build passes;
- macOS host build passes;
- host PTY functionality still works;
- PS4/PS5 target probe behavior is not weakened;
- diagnostics state honestly which mechanism was used.

# 2. PS4 cross-build CI failure

The current PS4 GitHub Actions configure step fails with an error equivalent to:

    ld.lld: cannot find linker script /ldscripts/elf_x86_64.x

Investigate the exact SDK release downloaded by CI.

The upstream PS4 SDK wrapper currently contains behavior where the linker wrapper refers to:

    PS4_PAYLOAD_SDK

when constructing the linker-script path.

The CI currently discovers:

    SDK_DIR

but does not necessarily export:

    PS4_PAYLOAD_SDK

for child toolchain processes.

Fix PSXTerm CI/environment setup.

DO NOT modify the upstream PS4 SDK repository.

DO NOT vendor a patched SDK into PSXTerm.

Prefer something equivalent to:

    PS4_PAYLOAD_SDK=<absolute sdk path>

being exported in the PS4 workflow job before CMake configure/build.

For PS5, keep the corresponding environment correct as well:

    PS5_PAYLOAD_SDK

if required.

Do not introduce brittle hard-coded `/home/runner/...` paths.

Use absolute paths derived at runtime.

Acceptance criteria:

    Payload build (ps4) -> success
    Payload build (ps5) -> success

# 3. Environment storage bug

Current psh/session environment uses a fixed variable-count limit:

    PSX_ENV_MAX = 64

and session initialization imports the daemon's inherited environment.

On environments such as GitHub Actions, the inherited process environment may consume most or all available entries.

This causes commands such as:

    export WHO=a
    export MARK=alpha

to fail because the table is full.

The integration test:

    test_multiple_sessions_are_isolated

then reports that the environment is missing from session A.

This is NOT fundamentally a session-isolation bug.

Fix the environment design.

# Environment requirements

Do NOT simply increase:

    PSX_ENV_MAX 64 -> 128
    PSX_ENV_MAX 64 -> 1024

That only postpones the bug.

Move toward dynamically-sized environment storage with explicit resource bounds.

For example:

    char **entries
    size_t count
    size_t capacity
    size_t bytes_used

Use a total memory budget.

Define something similar to:

    PSX_ENV_MAX_BYTES

with a reasonable bounded value.

Optionally retain an upper variable-count safety limit as a secondary guard, but it must not be the primary storage model.

Environment operations must support:

    set
    replace
    unset
    lookup
    iteration
    destruction

without leaks.

When replacing an existing variable, correctly update byte accounting.

When unsetting, correctly update byte accounting.

# Environment inheritance policy

Do not blindly assume every host environment variable is useful inside PSXTerm.

Review:

    psx_platform_inherit_environ()

and decide whether:

A. the entire environment should be inherited dynamically within the memory budget,

or

B. inheritance should be filtered.

Prefer preserving current behavior unless filtering has an actual technical reason.

Regardless, PSXTerm-defined variables must always be available:

    TERM
    PATH
    HOME
    USER
    SHELL=psh
    PSXTERM=1
    PWD

Important:

Inherited environment variables must NOT prevent core PSXTerm variables from existing.

# Environment tests

Add unit tests for:

- more than 64 variables;
- replacement of existing variables;
- unset;
- memory accounting;
- limit rejection;
- no leaks;
- PSXTerm variables surviving inheritance.

Fix integration tests so they also assert the return status of:

    export WHO=a
    export MARK=alpha

Do not allow a failed export to go unnoticed.

The session isolation test must verify both:

    A sees its own variable
    B does not see A's variable

# 4. Capability advertisement is currently semantically incorrect

PTTY capabilities must mean:

    THIS RUNNING DAEMON / SESSION CAN ACTUALLY PROVIDE THIS FEATURE NOW

not:

    some related code exists

Audit every capability bit.

Current capability definitions include:

    REAL_PTY
    PIPE_TTY
    EXEC
    FILE_TRANSFER
    SESSION_RESUME
    JOB_CONTROL
    AUTH_CHALLENGE
    COMPRESSION
    JSON_DIAGNOSTICS

# JOB_CONTROL bug

Current code advertises:

    PTTY_CAP_JOB_CONTROL

when:

    session->tty.is_real_pty

However actual psh job control is NOT implemented.

There are currently no complete:

    jobs
    fg
    bg

semantics and no finished process-group/job-control layer.

Therefore:

DO NOT advertise JOB_CONTROL.

A real PTY is a prerequisite for job control, not proof that job control exists.

Remove this capability until the actual feature is implemented and tested.

Update tests accordingly.

Do not add job control as part of this task.

# SESSION_RESUME correctness

Persistent sessions are configurable.

When the daemon runs with persistence disabled, for example:

    --no-persist

the session must NOT advertise:

    PTTY_CAP_SESSION_RESUME

because the advertised feature cannot actually be used.

Capability generation therefore needs access to both:

- runtime session properties;
- server feature/config state.

Do not solve this with global variables unless necessary.

Prefer an explicit session/runtime feature configuration.

Conceptually:

    psx_runtime_features_t

or equivalent.

For example:

    bool persistence_enabled;
    bool file_transfer_enabled;
    bool auth_challenge_enabled;
    bool job_control_enabled;

A session capability mask should be derived from:

    compiled feature
    AND server configuration
    AND runtime backend capability

Example:

    SESSION_RESUME =
        feature compiled
        && persistence_enabled

    REAL_PTY =
        tty backend is real PTY

    PIPE_TTY =
        tty backend is PipeTTY

    EXEC =
        process backend is actually available

    JOB_CONTROL =
        false

    FILE_TRANSFER =
        implemented and enabled

    JSON_DIAGNOSTICS =
        implemented

    AUTH_CHALLENGE =
        false

    COMPRESSION =
        false

Do not advertise future plans.

# Capability tests

Add/adjust tests covering:

Default daemon:

    SESSION_RESUME = true

Daemon with:

    --no-persist

must report:

    SESSION_RESUME = false

PipeTTY:

    PIPE_TTY = true
    REAL_PTY = false
    JOB_CONTROL = false

Real PTY:

    REAL_PTY = true

but still:

    JOB_CONTROL = false

until job control exists.

Compression:

    false

Auth challenge:

    false

Do not let README claim otherwise.

# 5. Review resume-token semantics

Do not redesign persistence.

Audit the existing resume-token implementation.

Verify:

- token generation uses strong random bytes where available;
- token is never logged by daemon;
- comparison remains constant-time for valid-length tokens;
- malformed lengths are rejected;
- attach to non-detached sessions is rejected;
- token is not accidentally exposed through `sessions`;
- temporary attach/session placeholder objects are always cleaned up.

Do not implement encryption or challenge-response auth here.

Only fix concrete bugs discovered during the audit.

# 6. Review file-transfer correctness

Do not add new file-transfer features.

Audit the current implementation for correctness.

Verify:

- uploads stream rather than buffer whole files;
- interrupted upload deletes temporary file;
- final destination is only published after successful completion;
- declared size is enforced;
- paths reject embedded NUL;
- oversized paths are rejected;
- concurrent transfer per session is bounded;
- disconnect/detach cleans state;
- file descriptors always close;
- zero-length files work;
- rename failure cleans temporary state appropriately.

# Important resumable-transfer wording

Current protocol includes offsets and FILE_SEEK, making future resume possible.

However do NOT claim:

    upload resume is implemented

unless an interrupted upload can actually reconnect and continue the temporary file safely.

If interrupted transfers currently delete the temporary upload, document this as:

    wire format is resume-friendly / future-resumable

NOT:

    resumable upload implemented

Keep current behavior unless a correctness bug requires changing it.

Do NOT implement reconnectable upload resume in this task.

# 7. CI must become the gate

Run and fix the actual workflows.

Required green checks:

    Host build and tests (ubuntu)
    Host build and tests (macOS)
    Sanitizers
    Payload build (PS4)
    Payload build (PS5)
    Docs Check

Do not merely make local tests green while GitHub Actions remains red.

When fixing a workflow problem, understand the cause.

Do not hide failures with:

    continue-on-error: true

Do not remove test jobs.

Do not skip macOS.

Do not skip PS4.

Do not turn failing checks into warnings.

# 8. Add regression tests for the discovered CI failures

Each discovered problem should have a regression guard where practical.

At minimum:

- environment >64 variables;
- capability `JOB_CONTROL` false;
- capability `SESSION_RESUME` false under `--no-persist`;
- host PTY tests not assuming TIOCGPTN exists universally.

For the PS4 SDK environment issue, the workflow itself is the regression test.

# 9. Documentation correction

After code and CI are green, update:

    README.md
    docs/IMPLEMENTATION.md
    docs/HARDWARE_BRINGUP.md

Only change statements affected by this work.

Explicitly keep:

    HARDWARE TEST REQUIRED

for PS4/PS5 runtime behavior.

Do NOT mark anything hardware-tested.

Ensure docs say:

- PS4 and PS5 cross-builds pass;
- host CI passes Linux/macOS;
- job control is NOT implemented;
- SESSION_RESUME depends on daemon persistence configuration;
- file transfer is implemented;
- interrupted uploads are cleaned up;
- reconnectable upload resume is NOT yet implemented unless that has genuinely changed.

# 10. Hardware artifact preparation

Once ALL CI is green, prepare the project for physical-console testing.

Do not perform hardware claims.

Ensure the build produces clearly identifiable artifacts:

PS5:

    psxtermd
    psxterm-ttyprobe
    cli_test.elf

PS4:

    psxtermd
    psxterm-ttyprobe
    cli_test.elf

Where appropriate ensure artifact naming cannot cause PS4 and PS5 payloads to be confused.

Consider packaging CI artifacts as:

    psxterm-ps5-<version>
    psxterm-ps4-<version>

but do not redesign releases unnecessarily.

# Hardware test sequence

Document this exact recommended sequence:

## Step 1

Run:

    psxterm-ttyprobe

on console.

Collect complete output.

## Step 2

Run:

    psxtermd --doctor --json

locally if supported by the loader environment.

Collect complete output.

## Step 3

Start:

    psxtermd

and connect from host.

Run:

    psxterm doctor --json <console-ip>

## Step 4

Open interactive session.

Verify:

    prompt
    pwd
    ls
    env
    resize

## Step 5

Run controlled:

    cli_test.elf

Verify:

    argv
    env
    stdin
    stdout
    stderr
    exit status
    isatty
    winsize

## Step 6

Verify:

    Ctrl+C / SIGINT

## Step 7

Only after the above:

    push
    pull
    detach
    attach

Do not make advanced workflows the first hardware test.

# Hardware failure discipline

Once physical logs are available, changes must be surgical.

Examples:

If:

    /dev/ptmx FAIL

work on PTY only.

If:

    PTY PASS
    CREATE_VICTIM FAIL

work on victim creation only.

If:

    CREATE_VICTIM PASS
    ATTACH FAIL

work on ptrace/attach only.

If:

    LOAD_ELF PASS
    DUP_STDIO FAIL

work on descriptor transfer only.

If:

    spawn PASS
    stdin FAIL

work on stdin only.

Do NOT redesign unrelated parts of PSXTerm because one target stage failed.

# No new feature creep

For this task specifically, DO NOT implement:

    HMAC auth
    challenge-response
    history
    completion
    job control
    pipelines
    redirection
    kqueue event loop
    Windows client
    transfer compression
    upload resume
    package management

These belong after physical console validation.

# Commit order

Keep commits focused.

Suggested sequence:

    fix: make host pty probe portable across linux and macos

    fix: export ps4 sdk root in payload ci

    refactor: make session environment dynamically bounded

    test: cover large inherited environments

    fix: advertise only implemented runtime capabilities

    test: cover no-persist capability semantics

    test: harden session and transfer regression coverage

    docs: prepare green-ci hardware bring-up state

Do not combine everything into one commit.

# No push rule

Do not push unless explicitly instructed.

Do not open a pull request unless explicitly instructed.

If the environment automatically operates on a working tree, leave changes committed locally only if that matches the existing workflow.

# Definition of done

This task is DONE only when:

1. current repository has been audited;
2. macOS host build passes;
3. Linux host build passes;
4. all unit tests pass;
5. all integration tests pass;
6. ASan passes;
7. UBSan passes;
8. PS4 payload build passes;
9. PS5 payload build passes;
10. JOB_CONTROL is not falsely advertised;
11. SESSION_RESUME reflects actual persistence configuration;
12. environment storage no longer fails merely because inherited environment exceeds 64 variables;
13. documentation matches actual behavior;
14. hardware artifacts are ready;
15. PS4/PS5 runtime status remains HARDWARE TEST REQUIRED.

Then STOP.

Do NOT continue into another feature phase.

The next step after this task is physical PS4/PS5 testing and analysis of the resulting logs.