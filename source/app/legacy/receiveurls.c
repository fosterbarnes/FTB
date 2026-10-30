#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <3ds.h>

#include "../resources.h"
#include "../screens.h"
#include "../action/action.h"
#include "../../core/core.h"

// FBI's Remote Install "Receive URLs over the network": FBI's PC tools (servefiles.py, sendurls.py,
// Boop) connect to port 5000 and send a big-endian length plus newline-separated URLs; one byte goes
// back once the install prompt or batch is done.

#define RECEIVEURLS_PORT 5000
// The transfer runs on the UI thread: a sender that stops for this long is dropped.
#define RECEIVEURLS_STALL_MS 10000
#define RECEIVEURLS_POLL_MS 100

typedef struct {
    // -1 = not open.
    int serverSocket;
    int clientSocket;
} receiveurls_data;

// Moves all len bytes, polling so B (errno ECANCELED) and a stalled sender (ETIMEDOUT) end it
// instead of freezing FTB. Returns len, or -1 with errno set.
static int receiveurls_transfer(int sockfd, void* buf, size_t len, bool receive) {
    size_t done = 0;
    u64 lastProgress = osGetTime();
    while(done < len) {
        hidScanInput();
        if(hidKeysDown() & KEY_B) {
            errno = ECANCELED;
            return -1;
        }

        if(osGetTime() - lastProgress >= RECEIVEURLS_STALL_MS) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct pollfd pfd = {sockfd, receive ? POLLIN : POLLOUT, 0};
        int ready = poll(&pfd, 1, RECEIVEURLS_POLL_MS);
        if(ready < 0) {
            return -1;
        }

        if(ready == 0) {
            continue;
        }

        int ret = receive ? recv(sockfd, (u8*) buf + done, len - done, 0) : send(sockfd, (u8*) buf + done, len - done, 0);
        if(ret < 0) {
            if(errno == EAGAIN) {
                continue;
            }

            return -1;
        }

        // The sender closed the connection early.
        if(ret == 0) {
            errno = ECONNRESET;
            return -1;
        }

        done += ret;
        lastProgress = osGetTime();
    }

    return (int) done;
}

static void receiveurls_close_client(void* data) {
    receiveurls_data* urlsData = (receiveurls_data*) data;

    if(urlsData->clientSocket >= 0) {
        u8 ack = 0;
        receiveurls_transfer(urlsData->clientSocket, &ack, sizeof(ack), false);

        close(urlsData->clientSocket);
        urlsData->clientSocket = -1;
    }
}

static void receiveurls_free_data(receiveurls_data* data) {
    receiveurls_close_client(data);

    if(data->serverSocket >= 0) {
        close(data->serverSocket);
        data->serverSocket = -1;
    }

    free(data);
}

// Shows why a receive failed; pressing B is not an error.
static void receiveurls_failed(const char* text) {
    if(errno != ECANCELED) {
        error_display_errno(NULL, NULL, errno, "%s", text);
    }
}

static void receiveurls_update(ui_view* view, void* data, float* progress, char* text) {
    receiveurls_data* urlsData = (receiveurls_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        info_destroy(view);

        receiveurls_free_data(urlsData);

        return;
    }

    // Still open only when action_install_url failed before its prompt (out of memory).
    receiveurls_close_client(urlsData);

    struct sockaddr_in client;
    socklen_t clientLen = sizeof(client);

    int sock = accept(urlsData->serverSocket, (struct sockaddr*) &client, &clientLen);
    int acceptErr = errno;
    if(sock >= 0) {
        urlsData->clientSocket = sock;

        u32 size = 0;
        if(receiveurls_transfer(urlsData->clientSocket, &size, sizeof(size), true) != sizeof(size)) {
            receiveurls_failed("Failed to read payload length.");

            receiveurls_close_client(data);
            return;
        }

        size = ntohl(size);
        if(size >= DOWNLOAD_URL_MAX * INSTALL_URLS_MAX) {
            error_display(NULL, NULL, "Payload too large.");

            receiveurls_close_client(data);
            return;
        }

        char* urls = (char*) calloc(size + 1, sizeof(char));
        if(urls == NULL) {
            error_display(NULL, NULL, "Failed to allocate URL buffer.");

            receiveurls_close_client(data);
            return;
        }

        if(receiveurls_transfer(urlsData->clientSocket, urls, size, true) != size) {
            receiveurls_failed("Failed to read URL(s).");

            free(urls);
            receiveurls_close_client(data);
            return;
        }

        action_install_url("Install from the received URL(s)?", urls, NULL, data, NULL, receiveurls_close_client, NULL);

        free(urls);
    } else if(acceptErr != EAGAIN) {
        // EAGAIN (newlib's EWOULDBLOCK too) only means no connection yet. Anything else (Wi-Fi off
        // or lost, a broken socket) would fail again every frame: show it once and close.
        ui_pop();
        info_destroy(view);

        error_display_errno(NULL, NULL, acceptErr, "Failed to open socket.");

        receiveurls_free_data(urlsData);
        return;
    }

    struct in_addr addr = {(in_addr_t) gethostid()};
    snprintf(text, PROGRESS_TEXT_MAX, "Waiting for connection...\nIP: %s\nPort: %d", inet_ntoa(addr), RECEIVEURLS_PORT);
}

void receiveurls_open() {
    receiveurls_data* data = (receiveurls_data*) calloc(1, sizeof(receiveurls_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate network install data.");

        return;
    }

    data->serverSocket = -1;
    data->clientSocket = -1;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(sock < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to open server socket.");

        receiveurls_free_data(data);
        return;
    }

    data->serverSocket = sock;

    // Reopening right after a transfer must not fail on the last connection's TIME_WAIT.
    int reuse = 1;
    setsockopt(data->serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(RECEIVEURLS_PORT);
    server.sin_addr.s_addr = (in_addr_t) gethostid();

    if(bind(data->serverSocket, (struct sockaddr*) &server, sizeof(server)) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to bind server socket.");

        receiveurls_free_data(data);
        return;
    }

    fcntl(data->serverSocket, F_SETFL, fcntl(data->serverSocket, F_GETFL, 0) | O_NONBLOCK);

    if(listen(data->serverSocket, 5) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to listen on server socket.");

        receiveurls_free_data(data);
        return;
    }

    info_display("Receive URL(s)", "B: Return", false, data, receiveurls_update, NULL);
}
