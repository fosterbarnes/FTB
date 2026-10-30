#pragma once

#include "../../core/task/task.h"

// HTTPS on 443 for one job: sending browsers that remember https://<ip> to the http:// page. The 3DS
// cannot get a trusted certificate for a LAN address, so it uses a self-signed one (kept in
// sdmc:/ftb/webtransfer.pem so a browser's "continue anyway" choice keeps working); browsers warn once,
// then follow the redirect.

typedef struct web_tls_s web_tls;

// Loads or makes the key and certificate. NULL when TLS is unavailable.
web_tls* web_tls_create(void);
void web_tls_free(web_tls* tls);
// Handshakes on the non-blocking client and answers its request with a redirect to location. Gives
// up on a silent or failed peer, after a deadline, or on cancel. The caller closes the client.
void web_tls_redirect(web_tls* tls, int client, const char* location, task_state* cancel);
