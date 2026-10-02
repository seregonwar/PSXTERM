#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "psxterm/session.h"

typedef enum {
    PSX_AUTH_NONE = 0, /* development mode: clearly insecure */
    PSX_AUTH_TOKEN = 1 /* shared token in the HELLO payload */
} psx_auth_mode_t;

typedef struct {
    uint16_t port;
    const char *bind_addr;
    size_t max_sessions;

    psx_auth_mode_t auth_mode;
    char auth_token[128];

    int handshake_timeout_ms;
    int idle_timeout_ms; /* 0 disables the idle timeout */

    /*
     * Session persistence: a dropped connection detaches the session instead
     * of destroying it. Detached sessions are reclaimed after
     * detached_timeout_ms (0 disables the reclamation).
     */
    bool persist_sessions;
    int detached_timeout_ms;

    bool dev_insecure; /* set when authentication is disabled on purpose */
} psx_server_config_t;

void psx_server_config_default(psx_server_config_t *config);

/* Runs the server until SIGINT/SIGTERM. Returns 0 on clean shutdown. */
int psx_server_run(const psx_server_config_t *config);
