/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#include "curl_setup.h"

#ifdef USE_NGTCP2
#include <ngtcp2/ngtcp2.h>
#include <nghttp3/nghttp3.h>

#ifdef USE_OPENSSL
#include <openssl/err.h>
#ifdef OPENSSL_IS_BORINGSSL
#include <ngtcp2/ngtcp2_crypto_boringssl.h>
#else
#include <ngtcp2/ngtcp2_crypto_openssl.h>
#endif
#include "vtls/openssl.h"
#elif defined(USE_GNUTLS)
#include <ngtcp2/ngtcp2_crypto_gnutls.h>
#include "vtls/gtls.h"
#elif defined(USE_WOLFSSL)
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#include "vtls/wolfssl.h"
#endif

#include "urldata.h"
#include "sendf.h"
#include "strdup.h"
#include "rand.h"
#include "multiif.h"
#include "strcase.h"
#include "cfilters.h"
#include "cf-socket.h"
#include "connect.h"
#include "strerror.h"
#include "dynbuf.h"
#include "vquic.h"
#include "h2h3.h"
#include "vtls/keylog.h"
#include "vtls/vtls.h"
#include "curl_ngtcp2.h"

/* The last 3 #include files should be in this order */
#include "curl_printf.h"
#include "curl_memory.h"
#include "memdebug.h"


#define H3_ALPN_H3_29 "\x5h3-29"
#define H3_ALPN_H3 "\x2h3"

/*
 * This holds outgoing HTTP/3 stream data that is used by nghttp3 until acked.
 * It is used as a circular buffer. Add new bytes at the end until it reaches
 * the far end, then start over at index 0 again.
 */

#define H3_SEND_SIZE (256*1024)
struct h3out {
  uint8_t buf[H3_SEND_SIZE];
  size_t used;   /* number of bytes used in the buffer */
  size_t windex; /* index in the buffer where to start writing the next
                    data block */
};

#define QUIC_MAX_STREAMS (256*1024)
#define QUIC_MAX_DATA (1*1024*1024)
#define QUIC_IDLE_TIMEOUT (60*NGTCP2_SECONDS)

#ifdef USE_OPENSSL
#define QUIC_CIPHERS                                                          \
  "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_"               \
  "POLY1305_SHA256:TLS_AES_128_CCM_SHA256"
#define QUIC_GROUPS "P-256:X25519:P-384:P-521"
#elif defined(USE_GNUTLS)
#define QUIC_PRIORITY \
  "NORMAL:-VERS-ALL:+VERS-TLS1.3:-CIPHER-ALL:+AES-128-GCM:+AES-256-GCM:" \
  "+CHACHA20-POLY1305:+AES-128-CCM:-GROUP-ALL:+GROUP-SECP256R1:" \
  "+GROUP-X25519:+GROUP-SECP384R1:+GROUP-SECP521R1:" \
  "%DISABLE_TLS13_COMPAT_MODE"
#elif defined(USE_WOLFSSL)
#define QUIC_CIPHERS                                                          \
  "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_"               \
  "POLY1305_SHA256:TLS_AES_128_CCM_SHA256"
#define QUIC_GROUPS "P-256:P-384:P-521"
#endif


/*
 * Store ngtcp2 version info in this buffer.
 */
void Curl_ngtcp2_ver(char *p, size_t len)
{
  const ngtcp2_info *ng2 = ngtcp2_version(0);
  const nghttp3_info *ht3 = nghttp3_version(0);
  (void)msnprintf(p, len, "ngtcp2/%s nghttp3/%s",
                  ng2->version_str, ht3->version_str);
}

struct blocked_pkt {
  const uint8_t *pkt;
  size_t pktlen;
  size_t gsolen;
};

struct cf_ngtcp2_ctx {
  curl_socket_t sockfd;
  struct sockaddr_storage local_addr;
  socklen_t local_addrlen;
  ngtcp2_conn *qconn;
  ngtcp2_cid dcid;
  ngtcp2_cid scid;
  uint32_t version;
  ngtcp2_settings settings;
  ngtcp2_transport_params transport_params;
  ngtcp2_connection_close_error last_error;
  ngtcp2_crypto_conn_ref conn_ref;
#ifdef USE_OPENSSL
  SSL_CTX *sslctx;
  SSL *ssl;
#elif defined(USE_GNUTLS)
  struct gtls_instance *gtls;
#elif defined(USE_WOLFSSL)
  WOLFSSL_CTX *sslctx;
  WOLFSSL *ssl;
#endif
  bool no_gso;
  uint8_t *pktbuf;
  size_t pktbuflen;
  /* the number of entries in blocked_pkt */
  size_t num_blocked_pkt;
  /* the number of processed entries in blocked_pkt */
  size_t num_blocked_pkt_sent;
  /* the packets blocked by sendmsg (EAGAIN or EWOULDBLOCK) */
  struct blocked_pkt blocked_pkt[2];

  nghttp3_conn *h3conn;
  nghttp3_settings h3settings;
  int qlogfd;
};


/* ngtcp2 default congestion controller does not perform pacing. Limit
   the maximum packet burst to MAX_PKT_BURST packets. */
#define MAX_PKT_BURST 10

static CURLcode cf_process_ingress(struct Curl_cfilter *cf,
                                   struct Curl_easy *data);
static CURLcode cf_flush_egress(struct Curl_cfilter *cf,
                                struct Curl_easy *data);
static int cb_h3_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
                                   uint64_t datalen, void *user_data,
                                   void *stream_user_data);

static ngtcp2_conn *get_conn(ngtcp2_crypto_conn_ref *conn_ref)
{
  struct Curl_cfilter *cf = conn_ref->user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  return ctx->qconn;
}

static ngtcp2_tstamp timestamp(void)
{
  struct curltime ct = Curl_now();
  return ct.tv_sec * NGTCP2_SECONDS + ct.tv_usec * NGTCP2_MICROSECONDS;
}

#ifdef DEBUG_NGTCP2
static void quic_printf(void *user_data, const char *fmt, ...)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;

  (void)ctx;  /* TODO: need an easy handle to infof() message */
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}
#endif

static void qlog_callback(void *user_data, uint32_t flags,
                          const void *data, size_t datalen)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  (void)flags;
  if(ctx->qlogfd != -1) {
    ssize_t rc = write(ctx->qlogfd, data, datalen);
    if(rc == -1) {
      /* on write error, stop further write attempts */
      close(ctx->qlogfd);
      ctx->qlogfd = -1;
    }
  }

}

static void quic_settings(struct cf_ngtcp2_ctx *ctx,
                          struct Curl_easy *data)
{
  ngtcp2_settings *s = &ctx->settings;
  ngtcp2_transport_params *t = &ctx->transport_params;
  ngtcp2_settings_default(s);
  ngtcp2_transport_params_default(t);
#ifdef DEBUG_NGTCP2
  s->log_printf = quic_printf;
#else
  s->log_printf = NULL;
#endif
  s->initial_ts = timestamp();
  t->initial_max_stream_data_bidi_local = data->set.buffer_size;
  t->initial_max_stream_data_bidi_remote = QUIC_MAX_STREAMS;
  t->initial_max_stream_data_uni = QUIC_MAX_STREAMS;
  t->initial_max_data = QUIC_MAX_DATA;
  t->initial_max_streams_bidi = 1;
  t->initial_max_streams_uni = 3;
  t->max_idle_timeout = QUIC_IDLE_TIMEOUT;
  if(ctx->qlogfd != -1) {
    s->qlog.write = qlog_callback;
  }
}

#ifdef USE_OPENSSL
static void keylog_callback(const SSL *ssl, const char *line)
{
  (void)ssl;
  Curl_tls_keylog_write_line(line);
}
#elif defined(USE_GNUTLS)
static int keylog_callback(gnutls_session_t session, const char *label,
                    const gnutls_datum_t *secret)
{
  gnutls_datum_t crandom;
  gnutls_datum_t srandom;

  gnutls_session_get_random(session, &crandom, &srandom);
  if(crandom.size != 32) {
    return -1;
  }

  Curl_tls_keylog_write(label, crandom.data, secret->data, secret->size);
  return 0;
}
#elif defined(USE_WOLFSSL)
#if defined(HAVE_SECRET_CALLBACK)
static void keylog_callback(const WOLFSSL *ssl, const char *line)
{
  (void)ssl;
  Curl_tls_keylog_write_line(line);
}
#endif
#endif

static int init_ngh3_conn(struct Curl_cfilter *cf);

#ifdef USE_OPENSSL
static CURLcode quic_ssl_ctx(SSL_CTX **pssl_ctx,
                             struct Curl_cfilter *cf, struct Curl_easy *data)
{
  struct connectdata *conn = cf->conn;
  CURLcode result = CURLE_FAILED_INIT;
  SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_method());

  if(!ssl_ctx) {
    result = CURLE_OUT_OF_MEMORY;
    goto out;
  }

#ifdef OPENSSL_IS_BORINGSSL
  if(ngtcp2_crypto_boringssl_configure_client_context(ssl_ctx) != 0) {
    failf(data, "ngtcp2_crypto_boringssl_configure_client_context failed");
    goto out;
  }
#else
  if(ngtcp2_crypto_openssl_configure_client_context(ssl_ctx) != 0) {
    failf(data, "ngtcp2_crypto_openssl_configure_client_context failed");
    goto out;
  }
#endif

  SSL_CTX_set_default_verify_paths(ssl_ctx);

#ifdef OPENSSL_IS_BORINGSSL
  if(SSL_CTX_set1_curves_list(ssl_ctx, QUIC_GROUPS) != 1) {
    failf(data, "SSL_CTX_set1_curves_list failed");
    goto out;
  }
#else
  if(SSL_CTX_set_ciphersuites(ssl_ctx, QUIC_CIPHERS) != 1) {
    char error_buffer[256];
    ERR_error_string_n(ERR_get_error(), error_buffer, sizeof(error_buffer));
    failf(data, "SSL_CTX_set_ciphersuites: %s", error_buffer);
    goto out;
  }

  if(SSL_CTX_set1_groups_list(ssl_ctx, QUIC_GROUPS) != 1) {
    failf(data, "SSL_CTX_set1_groups_list failed");
    goto out;
  }
#endif

  /* Open the file if a TLS or QUIC backend has not done this before. */
  Curl_tls_keylog_open();
  if(Curl_tls_keylog_enabled()) {
    SSL_CTX_set_keylog_callback(ssl_ctx, keylog_callback);
  }

  result = Curl_ssl_setup_x509_store(cf, data, ssl_ctx);
  if(result)
    goto out;

  /* OpenSSL always tries to verify the peer, this only says whether it should
   * fail to connect if the verification fails, or if it should continue
   * anyway. In the latter case the result of the verification is checked with
   * SSL_get_verify_result() below. */
  SSL_CTX_set_verify(ssl_ctx, conn->ssl_config.verifypeer ?
                     SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);

  /* give application a chance to interfere with SSL set up. */
  if(data->set.ssl.fsslctx) {
    Curl_set_in_callback(data, true);
    result = (*data->set.ssl.fsslctx)(data, ssl_ctx,
                                      data->set.ssl.fsslctxp);
    Curl_set_in_callback(data, false);
    if(result) {
      failf(data, "error signaled by ssl ctx callback");
      goto out;
    }
  }
  result = CURLE_OK;

out:
  *pssl_ctx = result? NULL : ssl_ctx;
  if(result && ssl_ctx)
    SSL_CTX_free(ssl_ctx);
  return result;
}

static CURLcode quic_set_client_cert(struct Curl_cfilter *cf,
                                     struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  SSL_CTX *ssl_ctx = ctx->sslctx;
  const struct ssl_config_data *ssl_config;

  ssl_config = Curl_ssl_get_config(data, FIRSTSOCKET);
  DEBUGASSERT(ssl_config);

  if(ssl_config->primary.clientcert || ssl_config->primary.cert_blob
     || ssl_config->cert_type) {
    return Curl_ossl_set_client_cert(
        data, ssl_ctx, ssl_config->primary.clientcert,
        ssl_config->primary.cert_blob, ssl_config->cert_type,
        ssl_config->key, ssl_config->key_blob,
        ssl_config->key_type, ssl_config->key_passwd);
  }

  return CURLE_OK;
}

/** SSL callbacks ***/

static CURLcode quic_init_ssl(struct Curl_cfilter *cf,
                              struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  const uint8_t *alpn = NULL;
  size_t alpnlen = 0;

  (void)data;
  DEBUGASSERT(!ctx->ssl);
  ctx->ssl = SSL_new(ctx->sslctx);

  SSL_set_app_data(ctx->ssl, &ctx->conn_ref);
  SSL_set_connect_state(ctx->ssl);
  SSL_set_quic_use_legacy_codepoint(ctx->ssl, 0);

  alpn = (const uint8_t *)H3_ALPN_H3_29 H3_ALPN_H3;
  alpnlen = sizeof(H3_ALPN_H3_29) - 1 + sizeof(H3_ALPN_H3) - 1;
  if(alpn)
    SSL_set_alpn_protos(ctx->ssl, alpn, (int)alpnlen);

  /* set SNI */
  SSL_set_tlsext_host_name(ctx->ssl, cf->conn->host.name);
  return CURLE_OK;
}
#elif defined(USE_GNUTLS)
static CURLcode quic_init_ssl(struct Curl_cfilter *cf,
                              struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  CURLcode result;
  gnutls_datum_t alpn[2];
  /* this will need some attention when HTTPS proxy over QUIC get fixed */
  const char * const hostname = cf->conn->host.name;
  long * const pverifyresult = &data->set.ssl.certverifyresult;
  int rc;

  DEBUGASSERT(ctx->gtls == NULL);
  ctx->gtls = calloc(1, sizeof(*(ctx->gtls)));
  if(!ctx->gtls)
    return CURLE_OUT_OF_MEMORY;

  result = gtls_client_init(data, &cf->conn->ssl_config, &data->set.ssl,
                            hostname, ctx->gtls, pverifyresult);
  if(result)
    return result;

  gnutls_session_set_ptr(ctx->gtls->session, &ctx->conn_ref);

  if(ngtcp2_crypto_gnutls_configure_client_session(ctx->gtls->session) != 0) {
    DEBUGF(LOG_CF(data, cf,
                  "ngtcp2_crypto_gnutls_configure_client_session failed\n"));
    return CURLE_QUIC_CONNECT_ERROR;
  }

  rc = gnutls_priority_set_direct(ctx->gtls->session, QUIC_PRIORITY, NULL);
  if(rc < 0) {
    DEBUGF(LOG_CF(data, cf, "gnutls_priority_set_direct failed: %s\n",
                  gnutls_strerror(rc)));
    return CURLE_QUIC_CONNECT_ERROR;
  }

  /* Open the file if a TLS or QUIC backend has not done this before. */
  Curl_tls_keylog_open();
  if(Curl_tls_keylog_enabled()) {
    gnutls_session_set_keylog_function(ctx->gtls->session, keylog_callback);
  }

  /* strip the first byte (the length) from NGHTTP3_ALPN_H3 */
  alpn[0].data = (unsigned char *)H3_ALPN_H3_29 + 1;
  alpn[0].size = sizeof(H3_ALPN_H3_29) - 2;
  alpn[1].data = (unsigned char *)H3_ALPN_H3 + 1;
  alpn[1].size = sizeof(H3_ALPN_H3) - 2;

  gnutls_alpn_set_protocols(ctx->gtls->session,
                            alpn, 2, GNUTLS_ALPN_MANDATORY);
  return CURLE_OK;
}
#elif defined(USE_WOLFSSL)

static CURLcode quic_ssl_ctx(WOLFSSL_CTX **pssl_ctx,
                             struct Curl_cfilter *cf, struct Curl_easy *data)
{
  struct connectdata *conn = cf->conn;
  CURLcode result = CURLE_FAILED_INIT;
  WOLFSSL_CTX *ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());

  if(!ssl_ctx) {
    result = CURLE_OUT_OF_MEMORY;
    goto out;
  }

  if(ngtcp2_crypto_wolfssl_configure_client_context(ssl_ctx) != 0) {
    failf(data, "ngtcp2_crypto_wolfssl_configure_client_context failed");
    goto out;
  }

  wolfSSL_CTX_set_default_verify_paths(ssl_ctx);

  if(wolfSSL_CTX_set_cipher_list(ssl_ctx, QUIC_CIPHERS) != 1) {
    char error_buffer[256];
    ERR_error_string_n(ERR_get_error(), error_buffer, sizeof(error_buffer));
    failf(data, "SSL_CTX_set_ciphersuites: %s", error_buffer);
    goto out;
  }

  if(wolfSSL_CTX_set1_groups_list(ssl_ctx, (char *)QUIC_GROUPS) != 1) {
    failf(data, "SSL_CTX_set1_groups_list failed");
    goto out;
  }

  /* Open the file if a TLS or QUIC backend has not done this before. */
  Curl_tls_keylog_open();
  if(Curl_tls_keylog_enabled()) {
#if defined(HAVE_SECRET_CALLBACK)
    wolfSSL_CTX_set_keylog_callback(ssl_ctx, keylog_callback);
#else
    failf(data, "wolfSSL was built without keylog callback");
    goto out;
#endif
  }

  if(conn->ssl_config.verifypeer) {
    const char * const ssl_cafile = conn->ssl_config.CAfile;
    const char * const ssl_capath = conn->ssl_config.CApath;

    wolfSSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    if(conn->ssl_config.CAfile || conn->ssl_config.CApath) {
      /* tell wolfSSL where to find CA certificates that are used to verify
         the server's certificate. */
      if(!wolfSSL_CTX_load_verify_locations(ssl_ctx, ssl_cafile, ssl_capath)) {
        /* Fail if we insist on successfully verifying the server. */
        failf(data, "error setting certificate verify locations:"
              "  CAfile: %s CApath: %s",
              ssl_cafile ? ssl_cafile : "none",
              ssl_capath ? ssl_capath : "none");
        goto out;
      }
      infof(data, " CAfile: %s", ssl_cafile ? ssl_cafile : "none");
      infof(data, " CApath: %s", ssl_capath ? ssl_capath : "none");
    }
#ifdef CURL_CA_FALLBACK
    else {
      /* verifying the peer without any CA certificates won't work so
         use wolfssl's built-in default as fallback */
      wolfSSL_CTX_set_default_verify_paths(ssl_ctx);
    }
#endif
  }
  else {
    wolfSSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
  }

  /* give application a chance to interfere with SSL set up. */
  if(data->set.ssl.fsslctx) {
    Curl_set_in_callback(data, true);
    result = (*data->set.ssl.fsslctx)(data, ssl_ctx,
                                      data->set.ssl.fsslctxp);
    Curl_set_in_callback(data, false);
    if(result) {
      failf(data, "error signaled by ssl ctx callback");
      goto out;
    }
  }
  result = CURLE_OK;

out:
  *pssl_ctx = result? NULL : ssl_ctx;
  if(result && ssl_ctx)
    SSL_CTX_free(ssl_ctx);
  return result;
}

/** SSL callbacks ***/

static CURLcode quic_init_ssl(struct Curl_cfilter *cf,
                              struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  const uint8_t *alpn = NULL;
  size_t alpnlen = 0;
  /* this will need some attention when HTTPS proxy over QUIC get fixed */
  const char * const hostname = cf->conn->host.name;

  (void)data;
  DEBUGASSERT(!ctx->ssl);
  ctx->ssl = wolfSSL_new(ctx->sslctx);

  wolfSSL_set_app_data(ctx->ssl, &ctx->conn_ref);
  wolfSSL_set_connect_state(ctx->ssl);
  wolfSSL_set_quic_use_legacy_codepoint(ctx->ssl, 0);

  alpn = (const uint8_t *)H3_ALPN_H3_29 H3_ALPN_H3;
  alpnlen = sizeof(H3_ALPN_H3_29) - 1 + sizeof(H3_ALPN_H3) - 1;
  if(alpn)
    wolfSSL_set_alpn_protos(ctx->ssl, alpn, (int)alpnlen);

  /* set SNI */
  wolfSSL_UseSNI(ctx->ssl, WOLFSSL_SNI_HOST_NAME,
                 hostname, (unsigned short)strlen(hostname));

  return CURLE_OK;
}
#endif /* defined(USE_WOLFSSL) */

static int cb_handshake_completed(ngtcp2_conn *tconn, void *user_data)
{
  (void)user_data;
  (void)tconn;
  return 0;
}

static void extend_stream_window(ngtcp2_conn *tconn,
                                 struct HTTP *stream)
{
  size_t thismuch = stream->unacked_window;
  ngtcp2_conn_extend_max_stream_offset(tconn, stream->stream3_id, thismuch);
  ngtcp2_conn_extend_max_offset(tconn, thismuch);
  stream->unacked_window = 0;
}


static int cb_recv_stream_data(ngtcp2_conn *tconn, uint32_t flags,
                               int64_t stream_id, uint64_t offset,
                               const uint8_t *buf, size_t buflen,
                               void *user_data, void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  nghttp3_ssize nconsumed;
  int fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0;
  (void)offset;
  (void)stream_user_data;

  nconsumed =
    nghttp3_conn_read_stream(ctx->h3conn, stream_id, buf, buflen, fin);
  if(nconsumed < 0) {
    ngtcp2_connection_close_error_set_application_error(
        &ctx->last_error,
        nghttp3_err_infer_quic_app_error_code((int)nconsumed), NULL, 0);
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  /* number of bytes inside buflen which consists of framing overhead
   * including QPACK HEADERS. In other words, it does not consume payload of
   * DATA frame. */
  ngtcp2_conn_extend_max_stream_offset(tconn, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(tconn, nconsumed);

  return 0;
}

static int
cb_acked_stream_data_offset(ngtcp2_conn *tconn, int64_t stream_id,
                            uint64_t offset, uint64_t datalen, void *user_data,
                            void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  int rv;
  (void)stream_id;
  (void)tconn;
  (void)offset;
  (void)datalen;
  (void)stream_user_data;

  rv = nghttp3_conn_add_ack_offset(ctx->h3conn, stream_id, datalen);
  if(rv) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int cb_stream_close(ngtcp2_conn *tconn, uint32_t flags,
                           int64_t stream_id, uint64_t app_error_code,
                           void *user_data, void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  struct Curl_easy *data = stream_user_data;
  int rv;

  (void)data;
  (void)tconn;
  /* stream is closed... */

  if(!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) {
    app_error_code = NGHTTP3_H3_NO_ERROR;
  }

  rv = nghttp3_conn_close_stream(ctx->h3conn, stream_id,
                                 app_error_code);
  DEBUGF(LOG_CF(data, cf, "[qsid=%" PRIx64 "] close -> %d", stream_id, rv));
  if(rv) {
    ngtcp2_connection_close_error_set_application_error(
        &ctx->last_error, nghttp3_err_infer_quic_app_error_code(rv), NULL, 0);
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int cb_stream_reset(ngtcp2_conn *tconn, int64_t stream_id,
                           uint64_t final_size, uint64_t app_error_code,
                           void *user_data, void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  struct Curl_easy *data = stream_user_data;
  int rv;
  (void)tconn;
  (void)final_size;
  (void)app_error_code;
  (void)data;

  rv = nghttp3_conn_shutdown_stream_read(ctx->h3conn, stream_id);
  DEBUGF(LOG_CF(data, cf, "[h3sid=%" PRIx64 "] reset -> %d", stream_id, rv));
  if(rv) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int cb_stream_stop_sending(ngtcp2_conn *tconn, int64_t stream_id,
                                  uint64_t app_error_code, void *user_data,
                                  void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  int rv;
  (void)tconn;
  (void)app_error_code;
  (void)stream_user_data;

  rv = nghttp3_conn_shutdown_stream_read(ctx->h3conn, stream_id);
  if(rv) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int cb_extend_max_local_streams_bidi(ngtcp2_conn *tconn,
                                            uint64_t max_streams,
                                            void *user_data)
{
  (void)tconn;
  (void)max_streams;
  (void)user_data;

  return 0;
}

static int cb_extend_max_stream_data(ngtcp2_conn *tconn, int64_t stream_id,
                                     uint64_t max_data, void *user_data,
                                     void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  int rv;
  (void)tconn;
  (void)max_data;
  (void)stream_user_data;

  rv = nghttp3_conn_unblock_stream(ctx->h3conn, stream_id);
  if(rv) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static void cb_rand(uint8_t *dest, size_t destlen,
                    const ngtcp2_rand_ctx *rand_ctx)
{
  CURLcode result;
  (void)rand_ctx;

  result = Curl_rand(NULL, dest, destlen);
  if(result) {
    /* cb_rand is only used for non-cryptographic context.  If Curl_rand
       failed, just fill 0 and call it *random*. */
    memset(dest, 0, destlen);
  }
}

static int cb_get_new_connection_id(ngtcp2_conn *tconn, ngtcp2_cid *cid,
                                    uint8_t *token, size_t cidlen,
                                    void *user_data)
{
  CURLcode result;
  (void)tconn;
  (void)user_data;

  result = Curl_rand(NULL, cid->data, cidlen);
  if(result)
    return NGTCP2_ERR_CALLBACK_FAILURE;
  cid->datalen = cidlen;

  result = Curl_rand(NULL, token, NGTCP2_STATELESS_RESET_TOKENLEN);
  if(result)
    return NGTCP2_ERR_CALLBACK_FAILURE;

  return 0;
}

static int cb_recv_rx_key(ngtcp2_conn *tconn, ngtcp2_crypto_level level,
                          void *user_data)
{
  struct Curl_cfilter *cf = user_data;
  (void)tconn;

  if(level != NGTCP2_CRYPTO_LEVEL_APPLICATION) {
    return 0;
  }

  if(init_ngh3_conn(cf) != CURLE_OK) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static ngtcp2_callbacks ng_callbacks = {
  ngtcp2_crypto_client_initial_cb,
  NULL, /* recv_client_initial */
  ngtcp2_crypto_recv_crypto_data_cb,
  cb_handshake_completed,
  NULL, /* recv_version_negotiation */
  ngtcp2_crypto_encrypt_cb,
  ngtcp2_crypto_decrypt_cb,
  ngtcp2_crypto_hp_mask_cb,
  cb_recv_stream_data,
  cb_acked_stream_data_offset,
  NULL, /* stream_open */
  cb_stream_close,
  NULL, /* recv_stateless_reset */
  ngtcp2_crypto_recv_retry_cb,
  cb_extend_max_local_streams_bidi,
  NULL, /* extend_max_local_streams_uni */
  cb_rand,
  cb_get_new_connection_id,
  NULL, /* remove_connection_id */
  ngtcp2_crypto_update_key_cb, /* update_key */
  NULL, /* path_validation */
  NULL, /* select_preferred_addr */
  cb_stream_reset,
  NULL, /* extend_max_remote_streams_bidi */
  NULL, /* extend_max_remote_streams_uni */
  cb_extend_max_stream_data,
  NULL, /* dcid_status */
  NULL, /* handshake_confirmed */
  NULL, /* recv_new_token */
  ngtcp2_crypto_delete_crypto_aead_ctx_cb,
  ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
  NULL, /* recv_datagram */
  NULL, /* ack_datagram */
  NULL, /* lost_datagram */
  ngtcp2_crypto_get_path_challenge_data_cb,
  cb_stream_stop_sending,
  NULL, /* version_negotiation */
  cb_recv_rx_key,
  NULL, /* recv_tx_key */
  NULL, /* early_data_rejected */
};

static int cf_ngtcp2_get_select_socks(struct Curl_cfilter *cf,
                                      struct Curl_easy *data,
                                      curl_socket_t *socks)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  struct SingleRequest *k = &data->req;
  int rv = GETSOCK_BLANK;
  struct HTTP *stream = data->req.p.http;

  socks[0] = ctx->sockfd;

  /* in an HTTP/3 connection we can basically always get a frame so we should
     always be ready for one */
  rv |= GETSOCK_READSOCK(0);

  /* we're still uploading or the HTTP/2 layer wants to send data */
  if((k->keepon & (KEEP_SEND|KEEP_SEND_PAUSE)) == KEEP_SEND &&
     (!stream->h3out || stream->h3out->used < H3_SEND_SIZE) &&
     ngtcp2_conn_get_cwnd_left(ctx->qconn) &&
     ngtcp2_conn_get_max_data_left(ctx->qconn) &&
     nghttp3_conn_is_stream_writable(ctx->h3conn, stream->stream3_id))
    rv |= GETSOCK_WRITESOCK(0);

  return rv;
}

static int cb_h3_stream_close(nghttp3_conn *conn, int64_t stream_id,
                              uint64_t app_error_code, void *user_data,
                              void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct Curl_easy *data = stream_user_data;
  struct HTTP *stream = data->req.p.http;
  (void)conn;
  (void)stream_id;
  (void)app_error_code;
  (void)cf;

  DEBUGF(LOG_CF(data, cf, "[h3sid=%" PRIx64 "] close", stream_id));
  stream->closed = TRUE;
  stream->error3 = app_error_code;
  Curl_expire(data, 0, EXPIRE_QUIC);
  /* make sure that ngh3_stream_recv is called again to complete the transfer
     even if there are no more packets to be received from the server. */
  data->state.drain = 1;
  return 0;
}

/*
 * write_data() copies data to the stream's receive buffer. If not enough
 * space is available in the receive buffer, it copies the rest to the
 * stream's overflow buffer.
 */
static CURLcode write_data(struct HTTP *stream, const void *mem, size_t memlen)
{
  CURLcode result = CURLE_OK;
  const char *buf = mem;
  size_t ncopy = memlen;
  /* copy as much as possible to the receive buffer */
  if(stream->len) {
    size_t len = CURLMIN(ncopy, stream->len);
    memcpy(stream->mem, buf, len);
    stream->len -= len;
    stream->memlen += len;
    stream->mem += len;
    buf += len;
    ncopy -= len;
  }
  /* copy the rest to the overflow buffer */
  if(ncopy)
    result = Curl_dyn_addn(&stream->overflow, buf, ncopy);
  return result;
}

static int cb_h3_recv_data(nghttp3_conn *conn, int64_t stream_id,
                           const uint8_t *buf, size_t buflen,
                           void *user_data, void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct Curl_easy *data = stream_user_data;
  struct HTTP *stream = data->req.p.http;
  CURLcode result = CURLE_OK;
  (void)conn;
  (void)cf;

  result = write_data(stream, buf, buflen);
  DEBUGF(LOG_CF(data, cf, "[h3sid=%" PRIx64 "] recv_data(len=%zu) -> %d",
                stream_id, buflen, result));
  if(result) {
    return -1;
  }
  stream->unacked_window += buflen;
  (void)stream_id;
  (void)user_data;
  return 0;
}

static int cb_h3_deferred_consume(nghttp3_conn *conn, int64_t stream_id,
                                  size_t consumed, void *user_data,
                                  void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  (void)conn;
  (void)stream_user_data;
  (void)stream_id;

  ngtcp2_conn_extend_max_stream_offset(ctx->qconn, stream_id, consumed);
  ngtcp2_conn_extend_max_offset(ctx->qconn, consumed);
  return 0;
}

/* Decode HTTP status code.  Returns -1 if no valid status code was
   decoded. (duplicate from http2.c) */
static int decode_status_code(const uint8_t *value, size_t len)
{
  int i;
  int res;

  if(len != 3) {
    return -1;
  }

  res = 0;

  for(i = 0; i < 3; ++i) {
    char c = value[i];

    if(c < '0' || c > '9') {
      return -1;
    }

    res *= 10;
    res += c - '0';
  }

  return res;
}

static int cb_h3_end_headers(nghttp3_conn *conn, int64_t stream_id,
                             int fin, void *user_data, void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct Curl_easy *data = stream_user_data;
  struct HTTP *stream = data->req.p.http;
  CURLcode result = CURLE_OK;
  (void)conn;
  (void)stream_id;
  (void)fin;
  (void)cf;

  /* add a CRLF only if we've received some headers */
  if(stream->firstheader) {
    result = write_data(stream, "\r\n", 2);
    if(result) {
      return -1;
    }
  }

  DEBUGF(LOG_CF(data, cf, "[h3sid=%" PRIx64 "] end_headers(status_code=%d",
                stream_id, stream->status_code));
  if(stream->status_code / 100 != 1) {
    stream->bodystarted = TRUE;
  }
  return 0;
}

static int cb_h3_recv_header(nghttp3_conn *conn, int64_t stream_id,
                             int32_t token, nghttp3_rcbuf *name,
                             nghttp3_rcbuf *value, uint8_t flags,
                             void *user_data, void *stream_user_data)
{
  nghttp3_vec h3name = nghttp3_rcbuf_get_buf(name);
  nghttp3_vec h3val = nghttp3_rcbuf_get_buf(value);
  struct Curl_easy *data = stream_user_data;
  struct HTTP *stream = data->req.p.http;
  CURLcode result = CURLE_OK;
  (void)conn;
  (void)stream_id;
  (void)token;
  (void)flags;
  (void)user_data;

  if(token == NGHTTP3_QPACK_TOKEN__STATUS) {
    char line[14]; /* status line is always 13 characters long */
    size_t ncopy;
    stream->status_code = decode_status_code(h3val.base, h3val.len);
    DEBUGASSERT(stream->status_code != -1);
    ncopy = msnprintf(line, sizeof(line), "HTTP/3 %03d \r\n",
                      stream->status_code);
    result = write_data(stream, line, ncopy);
    if(result) {
      return -1;
    }
  }
  else {
    /* store as an HTTP1-style header */
    result = write_data(stream, h3name.base, h3name.len);
    if(result) {
      return -1;
    }
    result = write_data(stream, ": ", 2);
    if(result) {
      return -1;
    }
    result = write_data(stream, h3val.base, h3val.len);
    if(result) {
      return -1;
    }
    result = write_data(stream, "\r\n", 2);
    if(result) {
      return -1;
    }
  }

  stream->firstheader = TRUE;
  return 0;
}

static int cb_h3_stop_sending(nghttp3_conn *conn, int64_t stream_id,
                              uint64_t app_error_code, void *user_data,
                              void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  int rv;
  (void)conn;
  (void)stream_user_data;

  rv = ngtcp2_conn_shutdown_stream_read(ctx->qconn, stream_id, app_error_code);
  if(rv && rv != NGTCP2_ERR_STREAM_NOT_FOUND) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int cb_h3_reset_stream(nghttp3_conn *conn, int64_t stream_id,
                              uint64_t app_error_code, void *user_data,
                              void *stream_user_data) {
  struct Curl_cfilter *cf = user_data;
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  struct Curl_easy *data = stream_user_data;
  int rv;
  (void)conn;
  (void)data;

  rv = ngtcp2_conn_shutdown_stream_write(ctx->qconn, stream_id,
                                         app_error_code);
  DEBUGF(LOG_CF(data, cf, "[h3sid=%" PRIx64 "] reset -> %d", stream_id, rv));
  if(rv && rv != NGTCP2_ERR_STREAM_NOT_FOUND) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static nghttp3_callbacks ngh3_callbacks = {
  cb_h3_acked_stream_data, /* acked_stream_data */
  cb_h3_stream_close,
  cb_h3_recv_data,
  cb_h3_deferred_consume,
  NULL, /* begin_headers */
  cb_h3_recv_header,
  cb_h3_end_headers,
  NULL, /* begin_trailers */
  cb_h3_recv_header,
  NULL, /* end_trailers */
  cb_h3_stop_sending,
  NULL, /* end_stream */
  cb_h3_reset_stream,
  NULL /* shutdown */
};

static int init_ngh3_conn(struct Curl_cfilter *cf)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  CURLcode result;
  int rc;
  int64_t ctrl_stream_id, qpack_enc_stream_id, qpack_dec_stream_id;

  if(ngtcp2_conn_get_max_local_streams_uni(ctx->qconn) < 3) {
    return CURLE_QUIC_CONNECT_ERROR;
  }

  nghttp3_settings_default(&ctx->h3settings);

  rc = nghttp3_conn_client_new(&ctx->h3conn,
                               &ngh3_callbacks,
                               &ctx->h3settings,
                               nghttp3_mem_default(),
                               cf);
  if(rc) {
    result = CURLE_OUT_OF_MEMORY;
    goto fail;
  }

  rc = ngtcp2_conn_open_uni_stream(ctx->qconn, &ctrl_stream_id, NULL);
  if(rc) {
    result = CURLE_QUIC_CONNECT_ERROR;
    goto fail;
  }

  rc = nghttp3_conn_bind_control_stream(ctx->h3conn, ctrl_stream_id);
  if(rc) {
    result = CURLE_QUIC_CONNECT_ERROR;
    goto fail;
  }

  rc = ngtcp2_conn_open_uni_stream(ctx->qconn, &qpack_enc_stream_id, NULL);
  if(rc) {
    result = CURLE_QUIC_CONNECT_ERROR;
    goto fail;
  }

  rc = ngtcp2_conn_open_uni_stream(ctx->qconn, &qpack_dec_stream_id, NULL);
  if(rc) {
    result = CURLE_QUIC_CONNECT_ERROR;
    goto fail;
  }

  rc = nghttp3_conn_bind_qpack_streams(ctx->h3conn, qpack_enc_stream_id,
                                       qpack_dec_stream_id);
  if(rc) {
    result = CURLE_QUIC_CONNECT_ERROR;
    goto fail;
  }

  return CURLE_OK;
  fail:

  return result;
}

static size_t drain_overflow_buffer(struct HTTP *stream)
{
  size_t overlen = Curl_dyn_len(&stream->overflow);
  size_t ncopy = CURLMIN(overlen, stream->len);
  if(ncopy > 0) {
    memcpy(stream->mem, Curl_dyn_ptr(&stream->overflow), ncopy);
    stream->len -= ncopy;
    stream->mem += ncopy;
    stream->memlen += ncopy;
    if(ncopy != overlen)
      /* make the buffer only keep the tail */
      (void)Curl_dyn_tail(&stream->overflow, overlen - ncopy);
    else
      Curl_dyn_reset(&stream->overflow);
  }
  return ncopy;
}

/* incoming data frames on the h3 stream */
static ssize_t cf_ngtcp2_recv(struct Curl_cfilter *cf, struct Curl_easy *data,
                              char *buf, size_t len, CURLcode *err)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  struct HTTP *stream = data->req.p.http;

  DEBUGASSERT(cf->connected);
  DEBUGASSERT(ctx);
  DEBUGASSERT(ctx->qconn);
  DEBUGASSERT(ctx->h3conn);
  *err = CURLE_OK;

  if(!stream->memlen) {
    /* remember where to store incoming data for this stream and how big the
       buffer is */
    stream->mem = buf;
    stream->len = len;
  }
  /* else, there's data in the buffer already */

  /* if there's data in the overflow buffer from a previous call, copy as much
     as possible to the receive buffer before receiving more */
  drain_overflow_buffer(stream);

  if(cf_process_ingress(cf, data)) {
    *err = CURLE_RECV_ERROR;
    return -1;
  }
  if(cf_flush_egress(cf, data)) {
    *err = CURLE_SEND_ERROR;
    return -1;
  }

  if(stream->memlen) {
    ssize_t memlen = stream->memlen;
    /* data arrived */
    /* reset to allow more data to come */
    stream->memlen = 0;
    stream->mem = buf;
    stream->len = len;
    /* extend the stream window with the data we're consuming and send out
       any additional packets to tell the server that we can receive more */
    extend_stream_window(ctx->qconn, stream);
    if(cf_flush_egress(cf, data)) {
      *err = CURLE_SEND_ERROR;
      return -1;
    }
    return memlen;
  }

  if(stream->closed) {
    if(stream->error3 != NGHTTP3_H3_NO_ERROR) {
      failf(data,
            "HTTP/3 stream %" PRId64 " was not closed cleanly: (err %" PRIu64
            ")",
            stream->stream3_id, stream->error3);
      *err = CURLE_HTTP3;
      return -1;
    }

    if(!stream->bodystarted) {
      failf(data,
            "HTTP/3 stream %" PRId64 " was closed cleanly, but before getting"
            " all response header fields, treated as error",
            stream->stream3_id);
      *err = CURLE_HTTP3;
      return -1;
    }

    return 0;
  }

  infof(data, "ngh3_stream_recv returns 0 bytes and EAGAIN");
  *err = CURLE_AGAIN;
  return -1;
}

/* this amount of data has now been acked on this stream */
static int cb_h3_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
                                   uint64_t datalen, void *user_data,
                                   void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct Curl_easy *data = stream_user_data;
  struct HTTP *stream = data->req.p.http;
  (void)user_data;

  (void)cf;
  if(!data->set.postfields) {
    stream->h3out->used -= datalen;
    DEBUGF(LOG_CF(data, cf, "cb_h3_acked_stream_data, %"PRIu64" bytes, "
                  "%zd left unacked", datalen, stream->h3out->used));
    DEBUGASSERT(stream->h3out->used < H3_SEND_SIZE);

    if(stream->h3out->used == 0) {
      int rv = nghttp3_conn_resume_stream(conn, stream_id);
      if(rv) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
      }
    }
  }
  return 0;
}

static nghttp3_ssize cb_h3_readfunction(nghttp3_conn *conn, int64_t stream_id,
                                        nghttp3_vec *vec, size_t veccnt,
                                        uint32_t *pflags, void *user_data,
                                        void *stream_user_data)
{
  struct Curl_cfilter *cf = user_data;
  struct Curl_easy *data = stream_user_data;
  size_t nread;
  struct HTTP *stream = data->req.p.http;
  (void)cf;
  (void)conn;
  (void)stream_id;
  (void)user_data;
  (void)veccnt;

  if(data->set.postfields) {
    vec[0].base = data->set.postfields;
    vec[0].len = data->state.infilesize;
    *pflags = NGHTTP3_DATA_FLAG_EOF;
    return 1;
  }

  if(stream->upload_len && H3_SEND_SIZE <= stream->h3out->used) {
    return NGHTTP3_ERR_WOULDBLOCK;
  }

  nread = CURLMIN(stream->upload_len, H3_SEND_SIZE - stream->h3out->used);
  if(nread > 0) {
    /* nghttp3 wants us to hold on to the data until it tells us it is okay to
       delete it. Append the data at the end of the h3out buffer. Since we can
       only return consecutive data, copy the amount that fits and the next
       part comes in next invoke. */
    struct h3out *out = stream->h3out;
    if(nread + out->windex > H3_SEND_SIZE)
      nread = H3_SEND_SIZE - out->windex;

    memcpy(&out->buf[out->windex], stream->upload_mem, nread);

    /* that's the chunk we return to nghttp3 */
    vec[0].base = &out->buf[out->windex];
    vec[0].len = nread;

    out->windex += nread;
    out->used += nread;

    if(out->windex == H3_SEND_SIZE)
      out->windex = 0; /* wrap */
    stream->upload_mem += nread;
    stream->upload_len -= nread;
    if(data->state.infilesize != -1) {
      stream->upload_left -= nread;
      if(!stream->upload_left)
        *pflags = NGHTTP3_DATA_FLAG_EOF;
    }
    DEBUGF(LOG_CF(data, cf, "cb_h3_readfunction %zd bytes%s (at %zd unacked)",
                  nread, *pflags == NGHTTP3_DATA_FLAG_EOF?" EOF":"",
                  out->used));
  }
  if(stream->upload_done && !stream->upload_len &&
     (stream->upload_left <= 0)) {
    DEBUGF(LOG_CF(data, cf, "cb_h3_readfunction sets EOF"));
    *pflags = NGHTTP3_DATA_FLAG_EOF;
    return nread ? 1 : 0;
  }
  else if(!nread) {
    return NGHTTP3_ERR_WOULDBLOCK;
  }
  return 1;
}

/* Index where :authority header field will appear in request header
   field list. */
#define AUTHORITY_DST_IDX 3

static CURLcode h3_stream_open(struct Curl_cfilter *cf,
                               struct Curl_easy *data,
                               const void *mem,
                               size_t len)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  struct HTTP *stream = data->req.p.http;
  size_t nheader;
  CURLcode result = CURLE_OK;
  nghttp3_nv *nva = NULL;
  int64_t stream3_id;
  int rc;
  struct h3out *h3out = NULL;
  struct h2h3req *hreq = NULL;

  rc = ngtcp2_conn_open_bidi_stream(ctx->qconn, &stream3_id, NULL);
  if(rc) {
    failf(data, "can get bidi streams");
    result = CURLE_SEND_ERROR;
    goto fail;
  }

  stream->stream3_id = stream3_id;
  stream->h3req = TRUE;
  Curl_dyn_init(&stream->overflow, CURL_MAX_READ_SIZE);

  result = Curl_pseudo_headers(data, mem, len, NULL, &hreq);
  if(result)
    goto fail;
  nheader = hreq->entries;

  nva = malloc(sizeof(nghttp3_nv) * nheader);
  if(!nva) {
    result = CURLE_OUT_OF_MEMORY;
    goto fail;
  }
  else {
    unsigned int i;
    for(i = 0; i < nheader; i++) {
      nva[i].name = (unsigned char *)hreq->header[i].name;
      nva[i].namelen = hreq->header[i].namelen;
      nva[i].value = (unsigned char *)hreq->header[i].value;
      nva[i].valuelen = hreq->header[i].valuelen;
      nva[i].flags = NGHTTP3_NV_FLAG_NONE;
    }
  }

  switch(data->state.httpreq) {
  case HTTPREQ_POST:
  case HTTPREQ_POST_FORM:
  case HTTPREQ_POST_MIME:
  case HTTPREQ_PUT: {
    nghttp3_data_reader data_reader;
    if(data->state.infilesize != -1)
      stream->upload_left = data->state.infilesize;
    else
      /* data sending without specifying the data amount up front */
      stream->upload_left = -1; /* unknown, but not zero */

    data_reader.read_data = cb_h3_readfunction;

    h3out = calloc(sizeof(struct h3out), 1);
    if(!h3out) {
      result = CURLE_OUT_OF_MEMORY;
      goto fail;
    }
    stream->h3out = h3out;

    rc = nghttp3_conn_submit_request(ctx->h3conn, stream->stream3_id,
                                     nva, nheader, &data_reader, data);
    if(rc) {
      result = CURLE_SEND_ERROR;
      goto fail;
    }
    break;
  }
  default:
    stream->upload_left = 0; /* nothing left to send */
    rc = nghttp3_conn_submit_request(ctx->h3conn, stream->stream3_id,
                                     nva, nheader, NULL, data);
    if(rc) {
      result = CURLE_SEND_ERROR;
      goto fail;
    }
    break;
  }

  Curl_safefree(nva);

  infof(data, "Using HTTP/3 Stream ID: %" PRIx64 " (easy handle %p)",
        stream3_id, (void *)data);

  Curl_pseudo_free(hreq);
  return CURLE_OK;

fail:
  free(nva);
  Curl_pseudo_free(hreq);
  return result;
}

static ssize_t cf_ngtcp2_send(struct Curl_cfilter *cf, struct Curl_easy *data,
                              const void *buf, size_t len, CURLcode *err)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  ssize_t sent = 0;
  struct HTTP *stream = data->req.p.http;

  DEBUGASSERT(cf->connected);
  DEBUGASSERT(ctx);
  DEBUGASSERT(ctx->qconn);
  DEBUGASSERT(ctx->h3conn);
  *err = CURLE_OK;

  if(stream->closed) {
    *err = CURLE_HTTP3;
    return -1;
  }

  if(!stream->h3req) {
    CURLcode result = h3_stream_open(cf, data, buf, len);
    if(result) {
      *err = CURLE_SEND_ERROR;
      return -1;
    }
    /* Assume that mem of length len only includes HTTP/1.1 style
       header fields.  In other words, it does not contain request
       body. */
    sent = len;
  }
  else {
    DEBUGF(LOG_CF(data, cf, "ngh3_stream_send() wants to send %zd bytes",
                  len));
    if(!stream->upload_len) {
      stream->upload_mem = buf;
      stream->upload_len = len;
      (void)nghttp3_conn_resume_stream(ctx->h3conn, stream->stream3_id);
    }
    else {
      *err = CURLE_AGAIN;
      return -1;
    }
  }

  if(cf_flush_egress(cf, data)) {
    *err = CURLE_SEND_ERROR;
    return -1;
  }

  /* Reset post upload buffer after resumed. */
  if(stream->upload_mem) {
    if(data->set.postfields) {
      sent = len;
    }
    else {
      sent = len - stream->upload_len;
    }

    stream->upload_mem = NULL;
    stream->upload_len = 0;

    if(sent == 0) {
      *err = CURLE_AGAIN;
      return -1;
    }
  }

  return sent;
}

static CURLcode qng_verify_peer(struct Curl_cfilter *cf,
                                struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  CURLcode result = CURLE_OK;
  const char *hostname, *disp_hostname;
  int port;
  char *snihost;

  Curl_conn_get_host(data, cf->sockindex, &hostname, &disp_hostname, &port);
  snihost = Curl_ssl_snihost(data, hostname, NULL);
  if(!snihost)
      return CURLE_PEER_FAILED_VERIFICATION;

  cf->conn->bits.multiplex = TRUE; /* at least potentially multiplexed */
  cf->conn->httpversion = 30;
  cf->conn->bundle->multiuse = BUNDLE_MULTIPLEX;

  if(cf->conn->ssl_config.verifyhost) {
#ifdef USE_OPENSSL
    X509 *server_cert;
    server_cert = SSL_get_peer_certificate(ctx->ssl);
    if(!server_cert) {
      return CURLE_PEER_FAILED_VERIFICATION;
    }
    result = Curl_ossl_verifyhost(data, cf->conn, server_cert);
    X509_free(server_cert);
    if(result)
      return result;
#elif defined(USE_GNUTLS)
    result = Curl_gtls_verifyserver(data, ctx->gtls->session,
                                    &cf->conn->ssl_config, &data->set.ssl,
                                    hostname, disp_hostname,
                                    data->set.str[STRING_SSL_PINNEDPUBLICKEY]);
    if(result)
      return result;
#elif defined(USE_WOLFSSL)
    if(wolfSSL_check_domain_name(ctx->ssl, snihost) == SSL_FAILURE)
      return CURLE_PEER_FAILED_VERIFICATION;
#endif
    infof(data, "Verified certificate just fine");
  }
  else
    infof(data, "Skipped certificate verification");
#ifdef USE_OPENSSL
  if(data->set.ssl.certinfo)
    /* asked to gather certificate info */
    (void)Curl_ossl_certchain(data, ctx->ssl);
#endif
  return result;
}

static CURLcode cf_process_ingress(struct Curl_cfilter *cf,
                                   struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  ssize_t recvd;
  int rv;
  uint8_t buf[65536];
  size_t bufsize = sizeof(buf);
  struct sockaddr_storage remote_addr;
  socklen_t remote_addrlen;
  ngtcp2_path path;
  ngtcp2_tstamp ts = timestamp();
  ngtcp2_pkt_info pi = { 0 };

  for(;;) {
    remote_addrlen = sizeof(remote_addr);
    while((recvd = recvfrom(ctx->sockfd, (char *)buf, bufsize, 0,
                            (struct sockaddr *)&remote_addr,
                            &remote_addrlen)) == -1 &&
          SOCKERRNO == EINTR)
      ;
    if(recvd == -1) {
      if(SOCKERRNO == EAGAIN || SOCKERRNO == EWOULDBLOCK)
        break;
      if(SOCKERRNO == ECONNREFUSED) {
        const char *r_ip;
        int r_port;
        Curl_cf_socket_peek(cf->next, NULL, NULL, &r_ip, &r_port);
        failf(data, "ngtcp2: connection to %s port %u refused",
              r_ip, r_port);
        return CURLE_COULDNT_CONNECT;
      }
      failf(data, "ngtcp2: recvfrom() unexpectedly returned %zd (errno=%d)",
                  recvd, SOCKERRNO);
      return CURLE_RECV_ERROR;
    }

    ngtcp2_addr_init(&path.local, (struct sockaddr *)&ctx->local_addr,
                     ctx->local_addrlen);
    ngtcp2_addr_init(&path.remote, (struct sockaddr *)&remote_addr,
                     remote_addrlen);

    rv = ngtcp2_conn_read_pkt(ctx->qconn, &path, &pi, buf, recvd, ts);
    if(rv) {
      if(!ctx->last_error.error_code) {
        if(rv == NGTCP2_ERR_CRYPTO) {
          ngtcp2_connection_close_error_set_transport_error_tls_alert(
              &ctx->last_error,
              ngtcp2_conn_get_tls_alert(ctx->qconn), NULL, 0);
        }
        else {
          ngtcp2_connection_close_error_set_transport_error_liberr(
              &ctx->last_error, rv, NULL, 0);
        }
      }

      if(rv == NGTCP2_ERR_CRYPTO)
        /* this is a "TLS problem", but a failed certificate verification
           is a common reason for this */
        return CURLE_PEER_FAILED_VERIFICATION;
      return CURLE_RECV_ERROR;
    }
  }

  return CURLE_OK;
}

static CURLcode do_sendmsg(struct Curl_cfilter *cf,
                           struct Curl_easy *data,
                           const uint8_t *pkt, size_t pktlen, size_t gsolen,
                           size_t *sent);

static CURLcode send_packet_no_gso(struct Curl_cfilter *cf,
                                   struct Curl_easy *data,
                                   const uint8_t *pkt, size_t pktlen,
                                   size_t gsolen, size_t *psent)
{
  const uint8_t *p, *end = pkt + pktlen;
  size_t sent;

  *psent = 0;

  for(p = pkt; p < end; p += gsolen) {
    size_t len = CURLMIN(gsolen, (size_t)(end - p));
    CURLcode curlcode = do_sendmsg(cf, data, p, len, len, &sent);
    if(curlcode != CURLE_OK) {
      return curlcode;
    }
    *psent += sent;
  }

  return CURLE_OK;
}

static CURLcode do_sendmsg(struct Curl_cfilter *cf,
                           struct Curl_easy *data,
                           const uint8_t *pkt, size_t pktlen, size_t gsolen,
                           size_t *psent)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
#ifdef HAVE_SENDMSG
  struct iovec msg_iov;
  struct msghdr msg = {0};
  ssize_t sent;
#if defined(__linux__) && defined(UDP_SEGMENT)
  uint8_t msg_ctrl[32];
  struct cmsghdr *cm;
#endif

  *psent = 0;
  msg_iov.iov_base = (uint8_t *)pkt;
  msg_iov.iov_len = pktlen;
  msg.msg_iov = &msg_iov;
  msg.msg_iovlen = 1;

#if defined(__linux__) && defined(UDP_SEGMENT)
  if(pktlen > gsolen) {
    /* Only set this, when we need it. macOS, for example,
     * does not seem to like a msg_control of length 0. */
    msg.msg_control = msg_ctrl;
    assert(sizeof(msg_ctrl) >= CMSG_SPACE(sizeof(uint16_t)));
    msg.msg_controllen = CMSG_SPACE(sizeof(uint16_t));
    cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    *(uint16_t *)(void *)CMSG_DATA(cm) = gsolen & 0xffff;
  }
#endif


  while((sent = sendmsg(ctx->sockfd, &msg, 0)) == -1 && SOCKERRNO == EINTR)
    ;

  if(sent == -1) {
    switch(SOCKERRNO) {
    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
#endif
      return CURLE_AGAIN;
    case EMSGSIZE:
      /* UDP datagram is too large; caused by PMTUD. Just let it be lost. */
      break;
    case EIO:
      if(pktlen > gsolen) {
        /* GSO failure */
        failf(data, "sendmsg() returned %zd (errno %d); disable GSO", sent,
              SOCKERRNO);
        ctx->no_gso = TRUE;
        return send_packet_no_gso(cf, data, pkt, pktlen, gsolen, psent);
      }
      /* FALLTHROUGH */
    default:
      failf(data, "sendmsg() returned %zd (errno %d)", sent, SOCKERRNO);
      return CURLE_SEND_ERROR;
    }
  }
  else {
    assert(pktlen == (size_t)sent);
  }
#else
  ssize_t sent;
  (void)gsolen;

  *psent = 0;

  while((sent = send(ctx->sockfd, (const char *)pkt, pktlen, 0)) == -1 &&
        SOCKERRNO == EINTR)
    ;

  if(sent == -1) {
    if(SOCKERRNO == EAGAIN || SOCKERRNO == EWOULDBLOCK) {
      return CURLE_AGAIN;
    }
    else {
      failf(data, "send() returned %zd (errno %d)", sent, SOCKERRNO);
      if(SOCKERRNO != EMSGSIZE) {
        return CURLE_SEND_ERROR;
      }
      /* UDP datagram is too large; caused by PMTUD. Just let it be
         lost. */
    }
  }
#endif

  *psent = pktlen;

  return CURLE_OK;
}

static CURLcode send_packet(struct Curl_cfilter *cf,
                            struct Curl_easy *data,
                            const uint8_t *pkt, size_t pktlen, size_t gsolen,
                            size_t *psent)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;

  if(ctx->no_gso && pktlen > gsolen) {
    return send_packet_no_gso(cf, data, pkt, pktlen, gsolen, psent);
  }

  return do_sendmsg(cf, data, pkt, pktlen, gsolen, psent);
}

static void push_blocked_pkt(struct Curl_cfilter *cf, const uint8_t *pkt,
                             size_t pktlen, size_t gsolen)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  struct blocked_pkt *blkpkt;

  assert(ctx->num_blocked_pkt <
         sizeof(ctx->blocked_pkt) / sizeof(ctx->blocked_pkt[0]));

  blkpkt = &ctx->blocked_pkt[ctx->num_blocked_pkt++];

  blkpkt->pkt = pkt;
  blkpkt->pktlen = pktlen;
  blkpkt->gsolen = gsolen;
}

static CURLcode send_blocked_pkt(struct Curl_cfilter *cf,
                                 struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  size_t sent;
  CURLcode curlcode;
  struct blocked_pkt *blkpkt;

  for(; ctx->num_blocked_pkt_sent < ctx->num_blocked_pkt;
      ++ctx->num_blocked_pkt_sent) {
    blkpkt = &ctx->blocked_pkt[ctx->num_blocked_pkt_sent];
    curlcode = send_packet(cf, data, blkpkt->pkt,
                           blkpkt->pktlen, blkpkt->gsolen, &sent);

    if(curlcode) {
      if(curlcode == CURLE_AGAIN) {
        blkpkt->pkt += sent;
        blkpkt->pktlen -= sent;
      }
      return curlcode;
    }
  }

  ctx->num_blocked_pkt = 0;
  ctx->num_blocked_pkt_sent = 0;

  return CURLE_OK;
}

static CURLcode cf_flush_egress(struct Curl_cfilter *cf,
                                struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  int rv;
  size_t sent;
  ngtcp2_ssize outlen;
  uint8_t *outpos = ctx->pktbuf;
  size_t max_udp_payload_size =
      ngtcp2_conn_get_max_tx_udp_payload_size(ctx->qconn);
  size_t path_max_udp_payload_size =
      ngtcp2_conn_get_path_max_tx_udp_payload_size(ctx->qconn);
  size_t max_pktcnt =
      CURLMIN(MAX_PKT_BURST, ctx->pktbuflen / max_udp_payload_size);
  size_t pktcnt = 0;
  size_t gsolen;
  ngtcp2_path_storage ps;
  ngtcp2_tstamp ts = timestamp();
  ngtcp2_tstamp expiry;
  ngtcp2_duration timeout;
  int64_t stream_id;
  nghttp3_ssize veccnt;
  int fin;
  nghttp3_vec vec[16];
  ngtcp2_ssize ndatalen;
  uint32_t flags;
  CURLcode curlcode;

  rv = ngtcp2_conn_handle_expiry(ctx->qconn, ts);
  if(rv) {
    failf(data, "ngtcp2_conn_handle_expiry returned error: %s",
          ngtcp2_strerror(rv));
    ngtcp2_connection_close_error_set_transport_error_liberr(&ctx->last_error,
                                                             rv, NULL, 0);
    return CURLE_SEND_ERROR;
  }

  if(ctx->num_blocked_pkt) {
    curlcode = send_blocked_pkt(cf, data);
    if(curlcode) {
      if(curlcode == CURLE_AGAIN) {
        Curl_expire(data, 1, EXPIRE_QUIC);
        return CURLE_OK;
      }
      return curlcode;
    }
  }

  ngtcp2_path_storage_zero(&ps);

  for(;;) {
    veccnt = 0;
    stream_id = -1;
    fin = 0;

    if(ctx->h3conn && ngtcp2_conn_get_max_data_left(ctx->qconn)) {
      veccnt = nghttp3_conn_writev_stream(ctx->h3conn, &stream_id, &fin, vec,
                                          sizeof(vec) / sizeof(vec[0]));
      if(veccnt < 0) {
        failf(data, "nghttp3_conn_writev_stream returned error: %s",
              nghttp3_strerror((int)veccnt));
        ngtcp2_connection_close_error_set_application_error(
            &ctx->last_error,
            nghttp3_err_infer_quic_app_error_code((int)veccnt), NULL, 0);
        return CURLE_SEND_ERROR;
      }
    }

    flags = NGTCP2_WRITE_STREAM_FLAG_MORE |
            (fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0);
    outlen = ngtcp2_conn_writev_stream(ctx->qconn, &ps.path, NULL, outpos,
                                       max_udp_payload_size,
                                       &ndatalen, flags, stream_id,
                                       (const ngtcp2_vec *)vec, veccnt, ts);
    if(outlen == 0) {
      if(outpos != ctx->pktbuf) {
        curlcode = send_packet(cf, data, ctx->pktbuf,
                               outpos - ctx->pktbuf, gsolen, &sent);
        if(curlcode) {
          if(curlcode == CURLE_AGAIN) {
            push_blocked_pkt(cf, ctx->pktbuf + sent,
                             outpos - ctx->pktbuf - sent,
                             gsolen);
            Curl_expire(data, 1, EXPIRE_QUIC);
            return CURLE_OK;
          }
          return curlcode;
        }
      }

      break;
    }
    if(outlen < 0) {
      switch(outlen) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
        assert(ndatalen == -1);
        nghttp3_conn_block_stream(ctx->h3conn, stream_id);
        continue;
      case NGTCP2_ERR_STREAM_SHUT_WR:
        assert(ndatalen == -1);
        nghttp3_conn_shutdown_stream_write(ctx->h3conn, stream_id);
        continue;
      case NGTCP2_ERR_WRITE_MORE:
        assert(ndatalen >= 0);
        rv = nghttp3_conn_add_write_offset(ctx->h3conn, stream_id, ndatalen);
        if(rv) {
          failf(data, "nghttp3_conn_add_write_offset returned error: %s\n",
                nghttp3_strerror(rv));
          return CURLE_SEND_ERROR;
        }
        continue;
      default:
        assert(ndatalen == -1);
        failf(data, "ngtcp2_conn_writev_stream returned error: %s",
              ngtcp2_strerror((int)outlen));
        ngtcp2_connection_close_error_set_transport_error_liberr(
            &ctx->last_error, (int)outlen, NULL, 0);
        return CURLE_SEND_ERROR;
      }
    }
    else if(ndatalen >= 0) {
      rv = nghttp3_conn_add_write_offset(ctx->h3conn, stream_id, ndatalen);
      if(rv) {
        failf(data, "nghttp3_conn_add_write_offset returned error: %s\n",
              nghttp3_strerror(rv));
        return CURLE_SEND_ERROR;
      }
    }

    outpos += outlen;

    if(pktcnt == 0) {
      gsolen = outlen;
    }
    else if((size_t)outlen > gsolen ||
            (gsolen > path_max_udp_payload_size &&
             (size_t)outlen != gsolen)) {
      /* Packet larger than path_max_udp_payload_size is PMTUD probe
         packet and it might not be sent because of EMSGSIZE. Send
         them separately to minimize the loss. */
      curlcode = send_packet(cf, data, ctx->pktbuf,
                             outpos - outlen - ctx->pktbuf, gsolen, &sent);
      if(curlcode) {
        if(curlcode == CURLE_AGAIN) {
          push_blocked_pkt(cf, ctx->pktbuf + sent,
                           outpos - outlen - ctx->pktbuf - sent, gsolen);
          push_blocked_pkt(cf, outpos - outlen, outlen, outlen);
          Curl_expire(data, 1, EXPIRE_QUIC);
          return CURLE_OK;
        }
        return curlcode;
      }
      curlcode = send_packet(cf, data, outpos - outlen, outlen,
                             outlen, &sent);
      if(curlcode) {
        if(curlcode == CURLE_AGAIN) {
          assert(0 == sent);
          push_blocked_pkt(cf, outpos - outlen, outlen, outlen);
          Curl_expire(data, 1, EXPIRE_QUIC);
          return CURLE_OK;
        }
        return curlcode;
      }

      pktcnt = 0;
      outpos = ctx->pktbuf;
      continue;
    }

    if(++pktcnt >= max_pktcnt || (size_t)outlen < gsolen) {
      curlcode = send_packet(cf, data, ctx->pktbuf,
                             outpos - ctx->pktbuf, gsolen, &sent);
      if(curlcode) {
        if(curlcode == CURLE_AGAIN) {
          push_blocked_pkt(cf, ctx->pktbuf + sent, outpos - ctx->pktbuf - sent,
                           gsolen);
          Curl_expire(data, 1, EXPIRE_QUIC);
          return CURLE_OK;
        }
        return curlcode;
      }

      pktcnt = 0;
      outpos = ctx->pktbuf;
    }
  }

  expiry = ngtcp2_conn_get_expiry(ctx->qconn);
  if(expiry != UINT64_MAX) {
    if(expiry <= ts) {
      timeout = 0;
    }
    else {
      timeout = expiry - ts;
      if(timeout % NGTCP2_MILLISECONDS) {
        timeout += NGTCP2_MILLISECONDS;
      }
    }
    Curl_expire(data, timeout / NGTCP2_MILLISECONDS, EXPIRE_QUIC);
  }

  return CURLE_OK;
}

/*
 * Called from transfer.c:data_pending to know if we should keep looping
 * to receive more data from the connection.
 */
static bool cf_ngtcp2_data_pending(struct Curl_cfilter *cf,
                                   const struct Curl_easy *data)
{
  /* We may have received more data than we're able to hold in the receive
     buffer and allocated an overflow buffer. Since it's possible that
     there's no more data coming on the socket, we need to keep reading
     until the overflow buffer is empty. */
  const struct HTTP *stream = data->req.p.http;
  (void)cf;
  return Curl_dyn_len(&stream->overflow) > 0;
}

static CURLcode cf_ngtcp2_data_event(struct Curl_cfilter *cf,
                                     struct Curl_easy *data,
                                     int event, int arg1, void *arg2)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  CURLcode result = CURLE_OK;

  (void)arg1;
  (void)arg2;
  switch(event) {
  case CF_CTRL_DATA_DONE: {
    struct HTTP *stream = data->req.p.http;
    Curl_dyn_free(&stream->overflow);
    free(stream->h3out);
    break;
  }

  case CF_CTRL_DATA_DONE_SEND: {
    struct HTTP *stream = data->req.p.http;
    stream->upload_done = TRUE;
    (void)nghttp3_conn_resume_stream(ctx->h3conn, stream->stream3_id);
    break;
  }

  case CF_CTRL_DATA_IDLE:
    if(timestamp() >= ngtcp2_conn_get_expiry(ctx->qconn)) {
      if(cf_flush_egress(cf, data)) {
        return CURLE_SEND_ERROR;
      }
    }
    break;
  default:
    break;
  }
  return result;
}

static void cf_ngtcp2_ctx_clear(struct cf_ngtcp2_ctx *ctx)
{
  if(ctx) {
    if(ctx->qlogfd != -1) {
      close(ctx->qlogfd);
      ctx->qlogfd = -1;
    }
#ifdef USE_OPENSSL
    if(ctx->ssl)
      SSL_free(ctx->ssl);
    if(ctx->sslctx)
      SSL_CTX_free(ctx->sslctx);
#elif defined(USE_GNUTLS)
    if(ctx->gtls) {
      if(ctx->gtls->cred)
        gnutls_certificate_free_credentials(ctx->gtls->cred);
      if(ctx->gtls->session)
        gnutls_deinit(ctx->gtls->session);
      free(ctx->gtls);
    }
#elif defined(USE_WOLFSSL)
    if(ctx->ssl)
      wolfSSL_free(ctx->ssl);
    if(ctx->sslctx)
      wolfSSL_CTX_free(ctx->sslctx);
#endif
    free(ctx->pktbuf);
    nghttp3_conn_del(ctx->h3conn);
    ngtcp2_conn_del(ctx->qconn);

    memset(ctx, 0, sizeof(*ctx));
  }
}

static void cf_ngtcp2_close(struct Curl_cfilter *cf, struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;

  (void)data;
  if(ctx && ctx->qconn) {
    char buffer[NGTCP2_MAX_UDP_PAYLOAD_SIZE];
    ngtcp2_tstamp ts;
    ngtcp2_ssize rc;

    ts = timestamp();
    rc = ngtcp2_conn_write_connection_close(ctx->qconn, NULL, /* path */
                                            NULL, /* pkt_info */
                                            (uint8_t *)buffer, sizeof(buffer),
                                            &ctx->last_error, ts);
    if(rc > 0) {
      while((send(ctx->sockfd, buffer, rc, 0) == -1) &&
            SOCKERRNO == EINTR);
    }

    cf_ngtcp2_ctx_clear(ctx);
  }

  cf->connected = FALSE;
}

static void cf_ngtcp2_destroy(struct Curl_cfilter *cf, struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;

  (void)data;
  cf_ngtcp2_ctx_clear(ctx);
  free(ctx);
  cf->ctx = NULL;
}

/*
 * Might be called twice for happy eyeballs.
 */
static CURLcode cf_connect_start(struct Curl_cfilter *cf,
                                 struct Curl_easy *data)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  int rc;
  int rv;
  CURLcode result;
  ngtcp2_path path; /* TODO: this must be initialized properly */
  const struct Curl_sockaddr_ex *sockaddr;
  const char *r_ip;
  int r_port;
  int qfd;

  result = Curl_cf_socket_peek(cf->next, &ctx->sockfd,
                               &sockaddr, &r_ip, &r_port);
  if(result)
    return result;
  DEBUGASSERT(ctx->sockfd != CURL_SOCKET_BAD);

  infof(data, "Connect socket %d over QUIC to %s:%d",
        ctx->sockfd, r_ip, r_port);

  rc = connect(ctx->sockfd, &sockaddr->sa_addr, sockaddr->addrlen);
  if(-1 == rc) {
    return Curl_socket_connect_result(data, r_ip, SOCKERRNO);
  }

  /* QUIC sockets need to be nonblocking */
  (void)curlx_nonblock(ctx->sockfd, TRUE);
  switch(sockaddr->family) {
#if defined(__linux__) && defined(IP_MTU_DISCOVER)
  case AF_INET: {
    int val = IP_PMTUDISC_DO;
    (void)setsockopt(ctx->sockfd, IPPROTO_IP, IP_MTU_DISCOVER, &val,
                     sizeof(val));
    break;
  }
#endif
#if defined(__linux__) && defined(IPV6_MTU_DISCOVER)
  case AF_INET6: {
    int val = IPV6_PMTUDISC_DO;
    (void)setsockopt(ctx->sockfd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val,
                     sizeof(val));
    break;
  }
#endif
  }

  ctx->version = NGTCP2_PROTO_VER_MAX;
#ifdef USE_OPENSSL
  result = quic_ssl_ctx(&ctx->sslctx, cf, data);
  if(result)
    return result;

  result = quic_set_client_cert(cf, data);
  if(result)
    return result;
#elif defined(USE_WOLFSSL)
  result = quic_ssl_ctx(&ctx->sslctx, cf, data);
  if(result)
    return result;
#endif

  result = quic_init_ssl(cf, data);
  if(result)
    return result;

  ctx->dcid.datalen = NGTCP2_MAX_CIDLEN;
  result = Curl_rand(data, ctx->dcid.data, NGTCP2_MAX_CIDLEN);
  if(result)
    return result;

  ctx->scid.datalen = NGTCP2_MAX_CIDLEN;
  result = Curl_rand(data, ctx->scid.data, NGTCP2_MAX_CIDLEN);
  if(result)
    return result;

  (void)Curl_qlogdir(data, ctx->scid.data, NGTCP2_MAX_CIDLEN, &qfd);
  ctx->qlogfd = qfd; /* -1 if failure above */
  quic_settings(ctx, data);

  ctx->local_addrlen = sizeof(ctx->local_addr);
  rv = getsockname(ctx->sockfd, (struct sockaddr *)&ctx->local_addr,
                   &ctx->local_addrlen);
  if(rv == -1)
    return CURLE_QUIC_CONNECT_ERROR;

  ngtcp2_addr_init(&path.local, (struct sockaddr *)&ctx->local_addr,
                   ctx->local_addrlen);
  ngtcp2_addr_init(&path.remote, &sockaddr->sa_addr, sockaddr->addrlen);

  rc = ngtcp2_conn_client_new(&ctx->qconn, &ctx->dcid, &ctx->scid, &path,
                              NGTCP2_PROTO_VER_V1, &ng_callbacks,
                              &ctx->settings, &ctx->transport_params,
                              NULL, cf);
  if(rc)
    return CURLE_QUIC_CONNECT_ERROR;

#ifdef USE_GNUTLS
  ngtcp2_conn_set_tls_native_handle(ctx->qconn, ctx->gtls->session);
#else
  ngtcp2_conn_set_tls_native_handle(ctx->qconn, ctx->ssl);
#endif

  ngtcp2_connection_close_error_default(&ctx->last_error);

#if defined(__linux__) && defined(UDP_SEGMENT) && defined(HAVE_SENDMSG)
  ctx->no_gso = FALSE;
#else
  ctx->no_gso = TRUE;
#endif

  ctx->num_blocked_pkt = 0;
  ctx->num_blocked_pkt_sent = 0;
  memset(&ctx->blocked_pkt, 0, sizeof(ctx->blocked_pkt));

  ctx->pktbuflen = NGTCP2_MAX_PMTUD_UDP_PAYLOAD_SIZE * MAX_PKT_BURST;
  ctx->pktbuf = malloc(ctx->pktbuflen);
  if(!ctx->pktbuf) {
    ngtcp2_conn_del(ctx->qconn);
    ctx->qconn = NULL;
    return CURLE_OUT_OF_MEMORY;
  }

  ctx->conn_ref.get_conn = get_conn;
  ctx->conn_ref.user_data = cf;

  return CURLE_OK;
}

static CURLcode cf_ngtcp2_connect(struct Curl_cfilter *cf,
                                  struct Curl_easy *data,
                                  bool blocking, bool *done)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;
  CURLcode result = CURLE_OK;

  if(cf->connected) {
    *done = TRUE;
    return CURLE_OK;
  }

  /* Connect the UDP filter first */
  if(!cf->next->connected) {
    result = Curl_conn_cf_connect(cf->next, data, blocking, done);
    if(result || !*done)
      return result;
  }

  *done = FALSE;
  if(!ctx->qconn) {
    result = cf_connect_start(cf, data);
    if(result)
      goto out;
  }

  result = cf_process_ingress(cf, data);
  if(result)
    goto out;

  result = cf_flush_egress(cf, data);
  if(result)
    goto out;

  if(ngtcp2_conn_get_handshake_completed(ctx->qconn)) {
    result = qng_verify_peer(cf, data);
    if(!result) {
      cf->connected = TRUE;
      cf->conn->alpn = CURL_HTTP_VERSION_3;
      *done = TRUE;
      connkeep(cf->conn, "HTTP/3 default");
    }
  }

out:
#ifndef CURL_DISABLE_VERBOSE_STRINGS
  if(result && result != CURLE_AGAIN) {
    const char *r_ip;
    int r_port;

    Curl_cf_socket_peek(cf->next, NULL, NULL, &r_ip, &r_port);
    infof(data, "connect to %s port %u failed: %s",
          r_ip, r_port, curl_easy_strerror(result));
  }
#endif
  return result;
}

static CURLcode cf_ngtcp2_query(struct Curl_cfilter *cf,
                                struct Curl_easy *data,
                                int query, int *pres1, void **pres2)
{
  struct cf_ngtcp2_ctx *ctx = cf->ctx;

  switch(query) {
  case CF_QUERY_MAX_CONCURRENT: {
    const ngtcp2_transport_params *rp;
    DEBUGASSERT(pres1);
    rp = ngtcp2_conn_get_remote_transport_params(ctx->qconn);
    if(rp)
      *pres1 = (rp->initial_max_streams_bidi > INT_MAX)?
                 INT_MAX : (int)rp->initial_max_streams_bidi;
    else  /* not arrived yet? */
      *pres1 = Curl_multi_max_concurrent_streams(data->multi);
    return CURLE_OK;
  }
  default:
    break;
  }
  return cf->next?
    cf->next->cft->query(cf->next, data, query, pres1, pres2) :
    CURLE_UNKNOWN_OPTION;
}


struct Curl_cftype Curl_cft_http3 = {
  "HTTP/3",
  CF_TYPE_IP_CONNECT | CF_TYPE_SSL | CF_TYPE_MULTIPLEX,
  0,
  cf_ngtcp2_destroy,
  cf_ngtcp2_connect,
  cf_ngtcp2_close,
  Curl_cf_def_get_host,
  cf_ngtcp2_get_select_socks,
  cf_ngtcp2_data_pending,
  cf_ngtcp2_send,
  cf_ngtcp2_recv,
  cf_ngtcp2_data_event,
  Curl_cf_def_conn_is_alive,
  Curl_cf_def_conn_keep_alive,
  cf_ngtcp2_query,
};

CURLcode Curl_cf_ngtcp2_create(struct Curl_cfilter **pcf,
                               struct Curl_easy *data,
                               struct connectdata *conn,
                               const struct Curl_addrinfo *ai)
{
  struct cf_ngtcp2_ctx *ctx = NULL;
  struct Curl_cfilter *cf = NULL, *udp_cf = NULL;
  CURLcode result;

  (void)data;
  (void)conn;
  ctx = calloc(sizeof(*ctx), 1);
  if(!ctx) {
    result = CURLE_OUT_OF_MEMORY;
    goto out;
  }

  result = Curl_cf_create(&cf, &Curl_cft_http3, ctx);
  if(result)
    goto out;

  result = Curl_cf_udp_create(&udp_cf, data, conn, ai);
  if(result)
    goto out;

  udp_cf->conn = cf->conn;
  udp_cf->sockindex = cf->sockindex;
  cf->next = udp_cf;

out:
  *pcf = (!result)? cf : NULL;
  if(result) {
    if(udp_cf)
      Curl_conn_cf_discard(udp_cf, data);
    Curl_safefree(cf);
    Curl_safefree(ctx);
  }

  return result;
}

bool Curl_conn_is_ngtcp2(const struct Curl_easy *data,
                         const struct connectdata *conn,
                         int sockindex)
{
  struct Curl_cfilter *cf = conn? conn->cfilter[sockindex] : NULL;

  (void)data;
  for(; cf; cf = cf->next) {
    if(cf->cft == &Curl_cft_http3)
      return TRUE;
    if(cf->cft->flags & CF_TYPE_IP_CONNECT)
      return FALSE;
  }
  return FALSE;
}

#endif
