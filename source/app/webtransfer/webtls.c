#include <errno.h>
#include <malloc.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <3ds.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "webtls.h"
#include "../paths.h"

#define WEB_TLS_PEM_PATH PATHS_DATA_DIR "/webtransfer.pem"
#define WEB_TLS_PEM_MAX 4096
#define WEB_TLS_HEAD_MAX 4096
#define WEB_TLS_POLL_MS 100
// Browsers open spare connections they may never use; one must not hold up the worker.
#define WEB_TLS_FIRST_BYTE_MS 1000
#define WEB_TLS_MS 5000

struct web_tls_s {
    mbedtls_entropy_context entropy; // sslcGenerateRandomData (the portlib's mbedtls_hardware_poll)
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509_crt cert;
    mbedtls_ssl_config config;
};

static bool web_tls_load(web_tls* tls) {
    FILE* fd = fopen(WEB_TLS_PEM_PATH, "rb");
    if(fd == NULL) {
        return false;
    }

    unsigned char pem[WEB_TLS_PEM_MAX];
    size_t size = fread(pem, 1, sizeof(pem) - 1, fd);
    fclose(fd);
    pem[size] = '\0';

    // PEM lengths count the terminating NUL.
    return size > 0 && mbedtls_pk_parse_key(&tls->key, pem, size + 1, NULL, 0) == 0 && mbedtls_x509_crt_parse(&tls->cert, pem, size + 1) == 0;
}

// A new P-256 key and self-signed certificate, saved for the next launch (a failed save only costs
// the browser's remembered choice).
static bool web_tls_generate(web_tls* tls) {
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);

    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);

    unsigned char pem[WEB_TLS_PEM_MAX];
    bool ok = mbedtls_pk_setup(&tls->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0
              && mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(tls->key), mbedtls_ctr_drbg_random, &tls->drbg) == 0
              // Random: Firefox refuses a certificate whose issuer and serial match another it has seen.
              && mbedtls_mpi_fill_random(&serial, 8, mbedtls_ctr_drbg_random, &tls->drbg) == 0;

    if(ok) {
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&writer, &tls->key);
        mbedtls_x509write_crt_set_issuer_key(&writer, &tls->key);

        ok = mbedtls_x509write_crt_set_subject_name(&writer, "CN=FTB 3DS Web Server") == 0
             && mbedtls_x509write_crt_set_issuer_name(&writer, "CN=FTB 3DS Web Server") == 0 && mbedtls_x509write_crt_set_serial(&writer, &serial) == 0
             && mbedtls_x509write_crt_set_validity(&writer, "20250101000000", "20491231235959") == 0
             && mbedtls_x509write_crt_pem(&writer, pem, sizeof(pem), mbedtls_ctr_drbg_random, &tls->drbg) == 0;
    }

    size_t certLength = ok ? strlen((char*) pem) : 0;
    ok = ok && mbedtls_pk_write_key_pem(&tls->key, pem + certLength, sizeof(pem) - certLength) == 0;

    size_t length = ok ? strlen((char*) pem) : 0;
    ok = ok && mbedtls_x509_crt_parse(&tls->cert, pem, length + 1) == 0;

    if(ok) {
        paths_ensure_data_dir();

        FILE* fd = fopen(WEB_TLS_PEM_PATH, "wb");
        if(fd != NULL) {
            fwrite(pem, 1, length, fd);
            fclose(fd);
        }
    }

    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&writer);

    return ok;
}

web_tls* web_tls_create(void) {
    if(R_FAILED(sslcInit(0))) {
        return NULL;
    }

    web_tls* tls = (web_tls*) calloc(1, sizeof(web_tls));
    if(tls == NULL) {
        sslcExit();
        return NULL;
    }

    mbedtls_entropy_init(&tls->entropy);
    mbedtls_ctr_drbg_init(&tls->drbg);
    mbedtls_pk_init(&tls->key);
    mbedtls_x509_crt_init(&tls->cert);
    mbedtls_ssl_config_init(&tls->config);

    static const char personal[] = "ftb web transfer";
    bool ok = mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func, &tls->entropy, (const unsigned char*) personal, sizeof(personal) - 1) == 0;

    if(ok && !web_tls_load(tls)) {
        mbedtls_pk_free(&tls->key);
        mbedtls_pk_init(&tls->key);
        mbedtls_x509_crt_free(&tls->cert);
        mbedtls_x509_crt_init(&tls->cert);

        ok = web_tls_generate(tls);
    }

    ok = ok && mbedtls_ssl_config_defaults(&tls->config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0
         && mbedtls_ssl_conf_own_cert(&tls->config, &tls->cert, &tls->key) == 0;

    if(!ok) {
        web_tls_free(tls);
        return NULL;
    }

    mbedtls_ssl_conf_rng(&tls->config, mbedtls_ctr_drbg_random, &tls->drbg);
    return tls;
}

void web_tls_free(web_tls* tls) {
    if(tls == NULL) {
        return;
    }

    mbedtls_ssl_config_free(&tls->config);
    mbedtls_x509_crt_free(&tls->cert);
    mbedtls_pk_free(&tls->key);
    mbedtls_ctr_drbg_free(&tls->drbg);
    mbedtls_entropy_free(&tls->entropy);
    free(tls);

    sslcExit();
}

static int web_tls_send(void* context, const unsigned char* buffer, size_t length) {
    int ret = send(*(int*) context, buffer, length, 0);
    return ret >= 0 ? ret : errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int web_tls_recv(void* context, unsigned char* buffer, size_t length) {
    int ret = recv(*(int*) context, buffer, length, 0);
    return ret >= 0 ? ret : errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

// After an mbedtls call returned ret: waits for the socket when it asked to, and says whether to call
// again. False on errors, the deadline, and cancel.
static bool web_tls_again(int fd, int ret, u64 deadline, task_state* cancel) {
    if((ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) || osGetTime() >= deadline || task_is_cancelled(cancel)) {
        return false;
    }

    struct pollfd pfd = {.fd = fd, .events = ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT};
    poll(&pfd, 1, WEB_TLS_POLL_MS);
    return true;
}

void web_tls_redirect(web_tls* tls, int client, const char* location, task_state* cancel) {
    u64 start = osGetTime();
    while(true) {
        struct pollfd pfd = {.fd = client, .events = POLLIN};
        if(poll(&pfd, 1, WEB_TLS_POLL_MS) > 0) {
            break;
        }

        if(osGetTime() - start >= WEB_TLS_FIRST_BYTE_MS || task_is_cancelled(cancel)) {
            return;
        }
    }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);

    u64 deadline = osGetTime() + WEB_TLS_MS;
    int ret = mbedtls_ssl_setup(&ssl, &tls->config);
    if(ret == 0) {
        mbedtls_ssl_set_bio(&ssl, &client, web_tls_send, web_tls_recv, NULL);
        while((ret = mbedtls_ssl_handshake(&ssl)) != 0 && web_tls_again(client, ret, deadline, cancel));
    }

    // Any request gets the same answer, so the head is only read to its end.
    char head[WEB_TLS_HEAD_MAX];
    size_t size = 0;
    while(ret == 0 && size < sizeof(head) - 1) {
        int read = mbedtls_ssl_read(&ssl, (unsigned char*) head + size, sizeof(head) - 1 - size);
        if(read > 0) {
            size += read;
            head[size] = '\0';
            if(strstr(head, "\r\n\r\n") != NULL) {
                break;
            }
        } else if(read == 0 || !web_tls_again(client, read, deadline, cancel)) {
            ret = -1;
        }
    }

    if(ret == 0) {
        char response[256];
        int length = snprintf(response, sizeof(response),
                              "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", location);

        int sent = 0;
        while(sent < length) {
            int written = mbedtls_ssl_write(&ssl, (const unsigned char*) response + sent, length - sent);
            if(written > 0) {
                sent += written;
            } else if(!web_tls_again(client, written, deadline, cancel)) {
                break;
            }
        }

        while((ret = mbedtls_ssl_close_notify(&ssl)) != 0 && web_tls_again(client, ret, deadline, cancel));
    }

    mbedtls_ssl_free(&ssl);
}
