#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <Block.h>

#include <sodium.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "dht/dht.h"

#include "log.h"
#include "lsd.h"
#include "utp.h"
#include "http.h"
#include "base64.h"
#include "timer.h"
#include "network.h"
#include "constants.h"
#include "bev_splice.h"
#include "hash_table.h"
#include "utp_bufferevent.h"


#define NO_DIRECT 0
#define NO_CACHE 0

typedef struct {
    in_addr_t ip;
    port_t port;
} PACKED address;

typedef struct {
    address addr;
    uint32_t last_verified;
    uint32_t last_connect;
    uint32_t last_connect_attempt;
} peer;

typedef struct {
    network *n;
    peer *peer;
    bufferevent *bev;
    evhttp_connection *evcon;
} peer_connection;

typedef struct {
    bool failed:1;
    uint32_t time_since_verified;
    uint32_t last_connect_attempt;
    bool never_connected:1;
    uint8_t salt;
    peer *peer;
} PACKED peer_sort;

#define CACHE_PATH "./cache/"
#define CACHE_NAME CACHE_PATH "cache.XXXXXXXX"

typedef void (^peer_connected)(peer_connection *p);
typedef struct pending_request {
    peer_connected connected;
    TAILQ_ENTRY(pending_request) next;
} pending_request;

typedef struct {
    network *n;

    evhttp_request *server_req;
    uint64 start_time;

    evhttp_request *direct_req;
    evhttp_connection *direct_req_evcon;

    evkeyvalq direct_headers;
    crypto_generichash_state content_state;

    uint8_t content_hash[crypto_generichash_BYTES];

    pending_request r;
    peer_connection *pc;
    evhttp_request *proxy_req;

    char cache_name[sizeof(CACHE_NAME)];
    int cache_file;

    bool dont_free:1;
} proxy_request;

typedef struct {
    uint length;
    peer *peers[];
} peer_array;

peer_array *injectors;
peer_array *injector_proxies;
peer_array *all_peers;

peer_connection *peer_connections[10];

char via_tag[] = "1.1 _.dcdn";
time_t last_trace;
time_t injector_reachable;

size_t pending_requests_len;
TAILQ_HEAD(, pending_request) pending_requests;


uint64_t us_clock()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

int mkpath(char *file_path)
{
    for (char *p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(file_path, 0755) == -1) {
            if (errno != EEXIST) {
                *p = '/';
                return -1;
            }
        }
        *p = '/';
    }
    return 0;
}

bool peer_is_injector(peer *p)
{
    for (uint i = 0; i < injectors->length; i++) {
        if (injectors->peers[i] == p) {
            return true;
        }
    }
    return false;
}

void on_utp_connect(network *n, peer_connection *pc)
{
    address *a = &pc->peer->addr;
    sockaddr_in sin = {.sin_family = AF_INET, .sin_addr.s_addr = a->ip, .sin_port = a->port};
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    getnameinfo((sockaddr*)&sin, sizeof(sin), host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST|NI_NUMERICSERV);
    debug("on_utp_connect %s:%s bev:%p\n", host, serv, pc->bev);
    bufferevent_disable(pc->bev, EV_READ|EV_WRITE);
    pc->evcon = evhttp_connection_base_bufferevent_new(n->evbase, n->evdns, pc->bev, host, atoi(serv));
    pc->bev = NULL;
    // handle waiting requests first
    if (!TAILQ_EMPTY(&pending_requests)) {
        pending_request *r = TAILQ_FIRST(&pending_requests);
        TAILQ_REMOVE(&pending_requests, r, next);
        pending_requests_len--;
        debug("on_utp_connect request:%p (outstanding:%zu)\n", r, pending_requests_len);
        bool found = false;
        for (uint i = 0; i < lenof(peer_connections); i++) {
            if (peer_connections[i] == pc) {
                peer_connections[i] = NULL;
                found = true;
            }
        }
        assert(found);
        debug("using new pc:%p for request:%p\n", pc, r);
        r->connected(pc);
        Block_release(r->connected);
        r->connected = NULL;
    }
}

void bev_event_cb(bufferevent *bufev, short events, void *arg)
{
    peer_connection *pc = (peer_connection *)arg;
    assert(pc->bev == bufev);
    debug("bev_event_cb pc:%p peer:%p events:0x%x\n", pc, pc->peer, events);
    if (events & (BEV_EVENT_EOF|BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT)) {
        bufferevent_free(pc->bev);
        pc->bev = NULL;
        if (peer_is_injector(pc->peer)) {
            injector_reachable = 0;
        }
        for (uint i = 0; i < lenof(peer_connections); i++) {
            if (peer_connections[i] == pc) {
                peer_connections[i] = NULL;
                break;
            }
        }
        assert(!pc->evcon);
        free(pc);
    } else if (events & BEV_EVENT_CONNECTED) {
        on_utp_connect(pc->n, pc);
    }
}

peer_connection* evhttp_utp_connect(network *n, peer *p)
{
    utp_socket *s = utp_create_socket(n->utp);
    address *a = &p->addr;
    sockaddr_in sin = {.sin_family = AF_INET, .sin_addr.s_addr = a->ip, .sin_port = a->port};
    debug("utp_socket_create_bev %s:%d\n", inet_ntoa((struct in_addr){.s_addr = a->ip}), ntohs(a->port));
    p->last_connect_attempt = time(NULL);
    peer_connection *pc = alloc(peer_connection);
    pc->n = n;
    pc->peer = p;
    pc->bev = utp_socket_create_bev(n->evbase, s);
    utp_connect(s, (sockaddr*)&sin, sizeof(sin));
    bufferevent_setcb(pc->bev, NULL, NULL, bev_event_cb, pc);
    bufferevent_enable(pc->bev, EV_READ);
    return pc;
}

void add_addresses(network *n, peer_array **pa, const uint8_t *addrs, uint num_addrs)
{
    for (uint i = 0; i < num_addrs; i++) {
        for (uint j = 0; j < (*pa)->length; j++) {
            if (memeq(&addrs[sizeof(address) * i], (const uint8_t *)&((*pa)->peers)[j]->addr, sizeof(address))) {
                return;
            }
        }
        address *a = (address *)&addrs[sizeof(address) * i];
        // paper over a bug in some DHT implementation that winds up with 1 for the port
        if (ntohs(a->port) == 1) {
            continue;
        }
        (*pa)->length++;
        *pa = realloc(*pa, sizeof(peer_array) + (*pa)->length * sizeof(peer*));
        peer *p = alloc(peer);
        (*pa)->peers[(*pa)->length-1] = p;
        p->addr = *a;
        const char *label = "peer";
        if (*pa == injectors) {
            label = "injector";
        } else if (*pa == injector_proxies) {
            label = "injector proxy";
        }
        debug("new %s %s:%d\n", label, inet_ntoa((struct in_addr){.s_addr = a->ip}), ntohs(a->port));

        sockaddr_in sin = {.sin_family = AF_INET, .sin_addr.s_addr = a->ip, .sin_port = a->port};
        dht_ping_node((const sockaddr *)&sin, sizeof(sin));

        if (!TAILQ_EMPTY(&pending_requests)) {
            for (uint k = 0; k < lenof(peer_connections); k++) {
                if (peer_connections[k]) {
                    continue;
                }
                peer_connections[k] = evhttp_utp_connect(n, p);
                break;
            }
        }
    }
}

void add_sockaddr(network *n, const sockaddr *addr, socklen_t addrlen)
{
    dht_ping_node(addr, addrlen);
    address a = {.ip = ((sockaddr_in*)addr)->sin_addr.s_addr, .port = ((sockaddr_in*)addr)->sin_port};
    add_addresses(n, &all_peers, (const byte*)&a, 1);
}

void dht_event_callback(void *closure, int event, const unsigned char *info_hash, const void *data, size_t data_len)
{
    network *n = (network*)closure;
    debug("dht_event_callback event:%d\n", event);
    // TODO: DHT_EVENT_VALUES6
    if (event != DHT_EVENT_VALUES) {
        return;
    }
    const uint8_t* peers = data;
    size_t num_peers = data_len / 6;
    debug("dht_event_callback num_peers:%zu\n", num_peers);
    if (memeq(info_hash, injector_swarm, sizeof(injector_swarm))) {
        add_addresses(n, &injectors, peers, num_peers);
    } else if (memeq(info_hash, injector_proxy_swarm, sizeof(injector_proxy_swarm))) {
        add_addresses(n, &injector_proxies, peers, num_peers);
    } else {
        add_addresses(n, &all_peers, peers, num_peers);
    }
    if (o_debug >= 2) {
        printf("Received %d values.\n", (int)(data_len / 6));
        printf("{\"");
        for (int j = 0; j < 20; j++) {
            printf("%02x", info_hash[j]);
        }
        printf("\": [");
        for (uint i = 0; i < data_len / 6; i++) {
            address *a = (address *)&data[i * 6];
            printf("\"%s:%d\"", inet_ntoa((struct in_addr){.s_addr = a->ip}), ntohs(a->port));
            if (i + 1 != data_len / 6) {
                printf(", ");
            }
        }
        printf("]}\n");
        if (data_len / 6 == 7) {
            dht_dump_tables(stdout);
        }
    }
}

void update_injector_proxy_swarm(network *n)
{
    if (injector_reachable) {
        dht_announce(n->dht, (const uint8_t *)injector_proxy_swarm);
    } else {
        dht_get_peers(n->dht, (const uint8_t *)injector_proxy_swarm);
    }
}

void abort_connect(pending_request *r)
{
    if (!r->connected) {
        return;
    }
    debug("aborting request:%p\n", r);
    Block_release(r->connected);
    r->connected = NULL;
    TAILQ_REMOVE(&pending_requests, r, next);
    pending_requests_len--;
    debug("abort_connect request:%p (outstanding:%zu)\n", r, pending_requests_len);
}

void peer_disconnect(peer_connection *pc)
{
    debug("disconnecting pc:%p\n", pc);
    if (pc->evcon) {
        evhttp_connection_free(pc->evcon);
        pc->evcon = NULL;
    }
    if (pc->bev) {
        bufferevent_free(pc->bev);
        pc->bev = NULL;
    }
    free(pc);
}

void proxy_cache_delete(proxy_request *p)
{
    if (p->cache_file != -1) {
        close(p->cache_file);
        p->cache_file = -1;
        unlink(p->cache_name);
    }
}

void proxy_request_cleanup(proxy_request *p)
{
    if (p->dont_free || p->proxy_req || p->direct_req || p->r.connected) {
        return;
    }
    if (p->server_req) {
        assert(!p->server_req->response_code);
        if (p->server_req->evcon) {
            evhttp_connection_set_closecb(p->server_req->evcon, NULL, NULL);
        }
        evhttp_send_error(p->server_req, 504, "Gateway Timeout");
        p->server_req = NULL;
    }
    if (p->pc) {
        peer_disconnect(p->pc);
        p->pc = NULL;
    }
    if (p->direct_req_evcon) {
        evhttp_connection_free(p->direct_req_evcon);
        p->direct_req_evcon = NULL;
    }
    evhttp_clear_headers(&p->direct_headers);
    proxy_cache_delete(p);
    free(p);
}

void peer_reuse(network *n, peer_connection *pc)
{
    // handle waiting requests first
    if (!TAILQ_EMPTY(&pending_requests)) {
        pending_request *r = TAILQ_FIRST(&pending_requests);
        TAILQ_REMOVE(&pending_requests, r, next);
        pending_requests_len--;
        debug("reusing pc:%p for request:%p (outstanding:%zu)\n", pc, r, pending_requests_len);
        r->connected(pc);
        Block_release(r->connected);
        r->connected = NULL;
        return;
    }
    // add to the pool if there's a slot
    for (uint i = 0; i < lenof(peer_connections); i++) {
        if (!peer_connections[i]) {
            debug("saving pc:%p for reuse\n", pc);
            peer_connections[i] = pc;
            return;
        }
    }
    // replace an in-progress connection if there is one
    for (uint i = 0; i < lenof(peer_connections); i++) {
        peer_connection *old_pc = peer_connections[i];
        if (!old_pc->evcon) {
            debug("replacing old_pc:%p with pc:%p\n", old_pc, pc);
            peer_disconnect(old_pc);
            peer_connections[i] = pc;
            return;
        }
    }
    // oh well
    peer_disconnect(pc);
}

double pdelta(proxy_request *p)
{
    return (double)(us_clock() - p->start_time) / 1000.0;
}

void proxy_cancel_direct(proxy_request *p)
{
    if (p->direct_req) {
        evhttp_cancel_request(p->direct_req);
        p->direct_req = NULL;
    }
}

void proxy_cancel_proxy(proxy_request *p)
{
    if (p->proxy_req) {
        proxy_cache_delete(p);
        evhttp_cancel_request(p->proxy_req);
        p->proxy_req = NULL;
    }
    if (!p->pc) {
        abort_connect(&p->r);
    } else {
        peer_disconnect(p->pc);
        p->pc = NULL;
    }
}

bool write_header_to_file(int headers_file, int code, const char *code_line, evkeyvalq *input_headers)
{
    const char *headers[] = hashed_headers;
    char buf[1024];
    int s = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\n", code, code_line);
    if (s >= (ssize_t)sizeof(buf)) {
        return false;
    }
    ssize_t w = write(headers_file, buf, s);
    if (s != w) {
        return false;
    }
    for (int i = 0; i < (int)lenof(headers) + 1; i++) {
        const char *key = i == lenof(headers) ? "X-Sign" : headers[i];
        const char *value = evhttp_find_header(input_headers, key);
        if (!value) {
            continue;
        }
        s = snprintf(buf, sizeof(buf), "%s: %s\r\n", key, value);
        if (s >= (ssize_t)sizeof(buf)) {
            return false;
        }
        w = write(headers_file, buf, s);
        if (s != w) {
            return false;
        }
    }
    write(headers_file, "\r\n", 2);
    return true;
}

void direct_chunked_cb(evhttp_request *req, void *arg);
void proxy_make_request(proxy_request *p);
void proxy_submit_request(proxy_request *p);

int direct_header_cb(evhttp_request *req, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    debug("p:%p (%.2fms) direct_header_cb %d %s %s\n", p, pdelta(p), req->response_code, req->response_code_line, evhttp_request_get_uri(p->server_req));
    proxy_cancel_proxy(p);
    p->direct_req_evcon = req->evcon;
    copy_all_headers(req, p->server_req);

    evhttp_add_header(req->input_headers, "Content-Location", evhttp_request_get_uri(p->server_req));

    evkeyval *header;
    TAILQ_FOREACH(header, req->input_headers, next) {
        evhttp_add_header(&p->direct_headers, header->key, header->value);
    }

    crypto_generichash_init(&p->content_state, NULL, 0, crypto_generichash_BYTES);
    hash_headers(req->input_headers, &p->content_state);

    evhttp_send_reply_start(p->server_req, req->response_code, req->response_code_line);

    snprintf(p->cache_name, sizeof(p->cache_name), CACHE_NAME);
    mkpath(p->cache_name);
    p->cache_file = mkstemp(p->cache_name);

    evhttp_request_set_chunked_cb(req, direct_chunked_cb);
    return 0;
}

void direct_chunked_cb(evhttp_request *req, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    evbuffer *input = req->input_buffer;
    //debug("p:%p direct_chunked_cb length:%zu\n", p, evbuffer_get_length(input));
    evbuffer_ptr ptr;
    evbuffer_iovec v;
    evbuffer_ptr_set(input, &ptr, 0, EVBUFFER_PTR_SET);
    while (evbuffer_peek(input, -1, &ptr, &v, 1) > 0) {
        crypto_generichash_update(&p->content_state, v.iov_base, v.iov_len);
        ssize_t w = write(p->cache_file, v.iov_base, v.iov_len);
        if (w != (ssize_t)v.iov_len) {
            fprintf(stderr, "p:%p cache write failed %d (%s)\n", p, errno, strerror(errno));
            proxy_cancel_proxy(p);
            break;
        }
        if (evbuffer_ptr_set(input, &ptr, v.iov_len, EVBUFFER_PTR_ADD) < 0) {
            break;
        }
    }
    if (p->server_req) {
        evhttp_send_reply_chunk(p->server_req, input);
    }
}

void direct_error_cb(enum evhttp_request_error error, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    debug("p:%p direct_error_cb %d\n", p, error);
    p->direct_req = NULL;
    proxy_request_cleanup(p);
}

void direct_request_done_cb(evhttp_request *req, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    debug("p:%p direct_request_done_cb %p\n", p, req);
    if (!req) {
        return;
    }
    debug("p:%p (%.2fms) direct server_request_done_cb %s\n", p, pdelta(p), evhttp_request_get_uri(p->server_req));
    if (req->response_code != 0) {
        crypto_generichash_final(&p->content_state, p->content_hash, sizeof(p->content_hash));

        return_connection(p->direct_req_evcon);
        p->direct_req_evcon = NULL;

        // submit a proxy-only request with If-None-Match: "base64(content_hash)" and let it cache
        assert(!p->proxy_req);
        proxy_make_request(p);
        size_t b64_hash_len;
        char *b64_hash = base64_urlsafe_encode((uint8_t*)&p->content_hash, sizeof(p->content_hash), &b64_hash_len);
        char etag[2048];
        snprintf(etag, sizeof(etag), "\"%s\"", b64_hash);
        free(b64_hash);
        debug("submitting a cache request %s\n", etag);
        evhttp_add_header(p->proxy_req->output_headers, "If-None-Match", etag);
        proxy_submit_request(p);

        if (p->server_req->evcon) {
            evhttp_connection_set_closecb(p->server_req->evcon, NULL, NULL);
        }
        evhttp_send_reply_end(p->server_req);
        p->server_req = NULL;
    }
    p->direct_req = NULL;
    if (!p->proxy_req) {
        proxy_request_cleanup(p);
    }
}

bool verify_signature(const uint8_t *content_hash, const char *sign)
{
    if (strlen(sign) != BASE64_LENGTH(sizeof(content_sig))) {
        fprintf(stderr, "Incorrect length! %zu != %zu\n", strlen(sign), sizeof(content_sig));
        return false;
    }

    size_t out_len = 0;
    uint8_t *raw_sig = base64_decode(sign, strlen(sign), &out_len);
    if (out_len != sizeof(content_sig)) {
        fprintf(stderr, "Incorrect length! %zu != %zu\n", out_len, sizeof(content_sig));
        free(raw_sig);
        return false;
    }

    unsigned char pk[crypto_sign_PUBLICKEYBYTES] = injector_pk;

    content_sig *sig = (content_sig*)raw_sig;
    if (crypto_sign_verify_detached(sig->signature, (uint8_t*)sig->sign, sizeof(content_sig) - sizeof(sig->signature), pk)) {
        fprintf(stderr, "Incorrect signature!\n");
        free(raw_sig);
        return false;
    }

    if (memcmp(content_hash, sig->content_hash, crypto_generichash_BYTES)) {
        fprintf(stderr, "Incorrect hash!\n");
        for (uint i = 0; i < crypto_generichash_BYTES; i++) {
            fprintf(stderr, "%02X", content_hash[i]);
        }
        fprintf(stderr, "\n");
        for (uint i = 0; i < sizeof(sig->content_hash); i++) {
            fprintf(stderr, "%02X", sig->content_hash[i]);
        }
        fprintf(stderr, "\n");
        free(raw_sig);
        return false;
    }

    free(raw_sig);
    return true;
}

bool addr_is_localhost(const sockaddr *sa, socklen_t salen)
{
    if (sa->sa_family == AF_INET) {
        const sockaddr_in *sin = (sockaddr_in *)sa;
        uint8_t *ip = (uint8_t*)&sin->sin_addr;
        return ip[0] == 127;
    }
    return false;
}

bool evcon_is_local_browser(evhttp_connection *evcon)
{
    int fd = bufferevent_getfd(evhttp_connection_get_bufferevent(evcon));
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    getsockname(fd, (sockaddr *)&ss, &len);
    // AF_LOCAL is from socketpair(), which means utp
    if (ss.ss_family == AF_LOCAL) {
        return false;
    }
    return addr_is_localhost((sockaddr *)&ss, len);
}

void copy_response_headers(evhttp_request *from, evhttp_request *to)
{
    const char *response_header_whitelist[] = hashed_headers;
    for (uint i = 0; i < lenof(response_header_whitelist); i++) {
        copy_header(from, to, response_header_whitelist[i]);
    }
    copy_header(from, to, "Content-Length");
    if (!evcon_is_local_browser(to->evcon)) {
        copy_header(from, to, "Content-Location");
        copy_header(from, to, "X-Sign");
    }
}

void proxy_send_error(proxy_request *p, int error, const char *reason)
{
    if (p->direct_req) {
        return;
    }
    if (p->server_req) {
        if (p->server_req->evcon) {
            evhttp_connection_set_closecb(p->server_req->evcon, NULL, NULL);
        }
        evhttp_send_error(p->server_req, error, reason);
        p->server_req = NULL;
    }
}

void proxy_chunked_cb(evhttp_request *req, void *arg);

int proxy_header_cb(evhttp_request *req, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    debug("p:%p (%.2fms) proxy_header_cb %d %s\n", p, pdelta(p), req->response_code, req->response_code_line);

    int klass = req->response_code / 100;
    switch (klass) {
    case 1:
    case 2:
    case 3:
        break;
    case 4:
    case 5:
        proxy_send_error(p, req->response_code, req->response_code_line);
    default:
        return -1;
    }

    // not the first moment of connection, but does indicate protocol support
    p->pc->peer->last_connect = time(NULL);

    if (p->cache_file != -1) {
        if (req->response_code == 304) {
            // have hash, file, and headers.
            return 0;
        }
        proxy_cache_delete(p);
    }

    if (p->cache_file == -1) {
        snprintf(p->cache_name, sizeof(p->cache_name), CACHE_NAME);
        mkpath(p->cache_name);
        p->cache_file = mkstemp(p->cache_name);
        debug("start cache:%s\n", p->cache_name);
    }

    crypto_generichash_init(&p->content_state, NULL, 0, crypto_generichash_BYTES);
    hash_headers(req->input_headers, &p->content_state);

    evhttp_request_set_chunked_cb(req, proxy_chunked_cb);
    return 0;
}

void proxy_chunked_cb(evhttp_request *req, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    evbuffer *input = req->input_buffer;
    //debug("p:%p proxy_chunked_cb length:%zu\n", p, evbuffer_get_length(input));
    evbuffer_ptr ptr;
    evbuffer_iovec v;
    evbuffer_ptr_set(input, &ptr, 0, EVBUFFER_PTR_SET);
    while (evbuffer_peek(input, -1, &ptr, &v, 1) > 0) {
        crypto_generichash_update(&p->content_state, v.iov_base, v.iov_len);
        ssize_t w = write(p->cache_file, v.iov_base, v.iov_len);
        if (w != (ssize_t)v.iov_len) {
            fprintf(stderr, "p:%p cache write failed %d (%s)\n", p, errno, strerror(errno));
            proxy_cancel_proxy(p);
            break;
        }
        if (evbuffer_ptr_set(input, &ptr, v.iov_len, EVBUFFER_PTR_ADD) < 0) {
            break;
        }
    }
}

void proxy_error_cb(enum evhttp_request_error error, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    debug("p:%p proxy_error_cb %d\n", p, error);
    if (error != EVREQ_HTTP_REQUEST_CANCEL && peer_is_injector(p->pc->peer)) {
        injector_reachable = 0;
    }
    p->proxy_req = NULL;
    proxy_request_cleanup(p);
}

char* cache_name_from_uri(const char *uri)
{
    size_t name_max = NAME_MAX - strlen(".headers");
    char *encoded_uri = evhttp_encode_uri(uri);
    if (strlen(encoded_uri) > name_max) {
        uint8_t uri_hash[crypto_generichash_BYTES];
        crypto_generichash(uri_hash, sizeof(uri_hash), (uint8_t*)uri, strlen(uri), NULL, 0);
        size_t b64_hash_len;
        char *b64_hash = base64_urlsafe_encode(uri_hash, sizeof(uri_hash), &b64_hash_len);
        encoded_uri[name_max - b64_hash_len - 2] = '.';
        strcpy(&encoded_uri[name_max - b64_hash_len - 1], b64_hash);
        free(b64_hash);
    }
    return encoded_uri;
}

void proxy_request_done_cb(evhttp_request *req, void *arg)
{
    proxy_request *p = (proxy_request*)arg;
    debug("p:%p proxy_request_done_cb req:%p\n", p, req);
    if (!req) {
        return;
    }
    if (!req->response_code) {
        p->proxy_req = NULL;
        proxy_request_cleanup(p);
        return;
    }

    const char *sign = evhttp_find_header(req->input_headers, "X-Sign");
    if (!sign) {
        fprintf(stderr, "no signature!\n");
        proxy_send_error(p, 502, "Missing Gateway Signature");
        p->proxy_req = NULL;
        proxy_request_cleanup(p);
        return;
    }
    if (req->chunk_cb) {
        crypto_generichash_final(&p->content_state, p->content_hash, sizeof(p->content_hash));
    }

    debug("verifying sig for %s %s\n", evhttp_request_get_uri(req), sign);
    if (!verify_signature(p->content_hash, sign)) {
        fprintf(stderr, "signature failed!\n");
        proxy_send_error(p, 502, "Bad Gateway Signature");
        p->proxy_req = NULL;
        proxy_request_cleanup(p);
        return;
    }
    fprintf(stderr, "signature good!\n");

    p->pc->peer->last_verified = time(NULL);
    if (peer_is_injector(p->pc->peer)) {
        injector_reachable = time(NULL);
        update_injector_proxy_swarm(p->n);
    }

    char headers_name[PATH_MAX];
    snprintf(headers_name, sizeof(headers_name), "%s.headers", p->cache_name);
    evkeyvalq *headers = req->input_headers;
    int code = req->response_code;
    const char *code_line = req->response_code_line;
    if (!req->chunk_cb) {
        assert(req->response_code == 304);
        code = 200;
        code_line = "OK";
        overwrite_kv_header(&p->direct_headers, "X-Sign", sign);
        headers = &p->direct_headers;
    }
    int headers_file = creat(headers_name, 0600);
    if (!write_header_to_file(headers_file, code, code_line, headers)) {
        unlink(headers_name);
    }
    fsync(headers_file);
    close(headers_file);

    const char *uri = evhttp_request_get_uri(req);
    char *encoded_uri = cache_name_from_uri(uri);
    char cache_path[PATH_MAX];
    char cache_headers_path[PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s%s", CACHE_PATH, encoded_uri);
    snprintf(cache_headers_path, sizeof(cache_headers_path), "%s.headers", cache_path);
    free(encoded_uri);
    debug("store cache:%s headers:%s\n", cache_path, cache_headers_path);

    fsync(p->cache_file);
    rename(p->cache_name, cache_path);
    rename(headers_name, cache_headers_path);

    if (p->server_req) {
        off_t length = lseek(p->cache_file, 0, SEEK_END);
        evbuffer *content = evbuffer_new();
        evbuffer_add_file(content, p->cache_file, 0, length);
        p->cache_file = -1;
        debug("p:%p (%.2fms) server_request_done_cb %d %s length:%u\n", p, pdelta(p),
            req->response_code, req->response_code_line, length);
        proxy_cancel_direct(p);
        copy_response_headers(req, p->server_req);
        if (p->server_req->evcon) {
            evhttp_connection_set_closecb(p->server_req->evcon, NULL, NULL);
        }
        evhttp_send_reply(p->server_req, req->response_code, req->response_code_line, content);
        p->server_req = NULL;
        evbuffer_free(content);
    }

    join_url_swarm(p->n, uri);
    peer_reuse(p->n, p->pc);
    p->pc = NULL;
    p->proxy_req = NULL;
    proxy_request_cleanup(p);
}

void direct_submit_request(proxy_request *p)
{
    assert(!p->direct_req);
    p->direct_req = evhttp_request_new(direct_request_done_cb, p);

    copy_all_headers(p->server_req, p->direct_req);

    evhttp_request_set_header_cb(p->direct_req, direct_header_cb);
    evhttp_request_set_error_cb(p->direct_req, direct_error_cb);

    char request_uri[2048];
    const evhttp_uri *uri = evhttp_request_get_evhttp_uri(p->server_req);
    const char *q = evhttp_uri_get_query(uri);
    const char *path = evhttp_uri_get_path(uri);
    if (!strlen(path)) {
        path = "/";
    }
    snprintf(request_uri, sizeof(request_uri), "%s%s%s", path, q?"?":"", q?q:"");
    evhttp_connection *evcon = make_connection(p->n, uri);
    evhttp_make_request(evcon, p->direct_req, EVHTTP_REQ_GET, request_uri);
    debug("p:%p con:%p direct request submitted: %s\n", p, p->direct_req->evcon, evhttp_request_get_uri(p->direct_req));
}

address parse_address(const char *addr)
{
    address a;
    char *port = strchr(addr, ':');
    *port = '\0';
    a.ip = inet_addr(addr);
    a.port = htons(atoi(port+1));
    return a;
}

void append_via(evhttp_request *from, evhttp_request *to)
{
    const char *via = evhttp_find_header(from->input_headers, "Via");
    char viab[2048];
    assert(!via || strlen(via) < sizeof(viab)/2);
    snprintf(viab, sizeof(viab), "%s%s%s", via?:"", via ? ", " : "", via_tag);
    overwrite_header(to, "Via", viab);
}

void proxy_make_request(proxy_request *p)
{
    assert(!p->proxy_req);
    p->proxy_req = evhttp_request_new(proxy_request_done_cb, p);
    const char *request_header_whitelist[] = {"Referer", "Host", "Via"};
    for (uint i = 0; i < lenof(request_header_whitelist); i++) {
        copy_header(p->server_req, p->proxy_req, request_header_whitelist[i]);
    }
    overwrite_header(p->proxy_req, "Proxy-Connection", "Keep-Alive");
    overwrite_header(p->proxy_req, "TE", "trailers");

    append_via(p->server_req, p->proxy_req);

    // TODO: range requests / partial content handling
    evhttp_remove_header(p->proxy_req->output_headers, "Range");
    evhttp_remove_header(p->proxy_req->output_headers, "If-Range");

    evhttp_request_set_header_cb(p->proxy_req, proxy_header_cb);
    evhttp_request_set_error_cb(p->proxy_req, proxy_error_cb);

    p->proxy_req->uri = strdup(evhttp_request_get_uri(p->server_req));
}

void proxy_submit_request_on_con(proxy_request *p, evhttp_connection *evcon)
{
    char *uri = p->proxy_req->uri;
    p->proxy_req->uri = NULL;
    evhttp_make_request(evcon, p->proxy_req, EVHTTP_REQ_GET, uri);
    free(uri);
    debug("p:%p con:%p proxy request submitted: %s\n", p, evhttp_request_get_connection(p->proxy_req), evhttp_request_get_uri(p->proxy_req));
}

int peer_sort_cmp(const peer_sort *pa, const peer_sort *pb)
{
    return memcmp(pa, pb, sizeof(peer_sort));
}

peer* select_peer(peer_array *pa)
{
    peer_sort best = {.peer = NULL};
    for (uint i = 0; i < pa->length; i++) {
        peer *p = pa->peers[i];
        peer_sort c;
        c.failed = p->last_connect < p->last_connect_attempt;
        c.time_since_verified = time(NULL) - p->last_verified;
        c.last_connect_attempt = p->last_connect_attempt;
        c.never_connected = !p->last_connect;
        c.salt = random() & 0xFF;
        c.peer = p;
        address *a = &p->addr;
        sockaddr_in sin = {.sin_family = AF_INET, .sin_addr.s_addr = a->ip, .sin_port = a->port};
        /*
        debug("peer %s:%d failed:%d verified_ago:%d last_connect:%d never_connected:%d salt:%d p:%p\n",
            inet_ntoa((struct in_addr){.s_addr = a->ip}), ntohs(a->port),
            c.failed, c.time_since_verified, c.last_connect_attempt, c.never_connected, c.salt, c.peer);
        */
        if (!i || peer_sort_cmp(&c, &best) < 0) {
            //debug("better p:%p\n", p);
            best = c;
        }
    }
    return best.peer;
}

peer_connection* start_peer_connection(network *n, peer_array *peers)
{
    peer *p = select_peer(peers);
    if (!p) {
        return NULL;
    }
    return evhttp_utp_connect(n, p);
}

void queue_request(network *n, pending_request *r, peer_connected connected)
{
    bool any_connected = false;
    for (uint i = 0; i < lenof(peer_connections); i++) {
        if (peer_connections[i]) {
            if (peer_connections[i]->evcon) {
                any_connected = true;
            }
            continue;
        }
        peer_connections[i] = start_peer_connection(n, all_peers);
        if (!peer_connections[i]) {
            break;
        }
    }

    static time_t last_lsd = 0;
    if (!any_connected && time(NULL) - last_lsd > 10) {
        last_lsd = time(NULL);
        lsd_send(n, false);
    }

    for (uint i = 0; i < lenof(peer_connections); i++) {
        peer_connection *pc = peer_connections[i];
        if (pc && pc->evcon) {
            peer_connections[i] = NULL;
            debug("using pc:%p for request:%p\n", pc, r);
            connected(pc);
            return;
        }
    }
    r->connected = Block_copy(connected);
    TAILQ_INSERT_TAIL(&pending_requests, r, next);
    pending_requests_len++;
    debug("queued request:%p (outstanding:%zu)\n", r, pending_requests_len);
}

void connect_more_injectors(network *n)
{
    for (uint i = 0; i < lenof(peer_connections); i++) {
        if (peer_connections[i]) {
            continue;
        }
        peer_array *o[2] = {injectors, injector_proxies};
        if (random() & 1) {
            o[0] = injector_proxies;
            o[1] = injectors;
        }
        peer_connections[i] = start_peer_connection(n, o[0]);
        if (!peer_connections[i]) {
            peer_connections[i] = start_peer_connection(n, o[1]);
        }
    }
}

void proxy_submit_request(proxy_request *p)
{
    if (!p->proxy_req) {
        proxy_make_request(p);
    }

    network *n = p->n;
    fetch_url_swarm(p->n, evhttp_request_get_uri(p->proxy_req));

    queue_request(p->n, &p->r, ^(peer_connection *pc) {
        p->pc = pc;
        proxy_submit_request_on_con(p, p->pc->evcon);
    });
}

void server_evcon_close_cb(evhttp_connection *evcon, void *ctx)
{
    proxy_request *p = (proxy_request*)ctx;
    debug("p:%p server_evcon_close_cb\n", p);
    evhttp_connection_set_closecb(evcon, NULL, NULL);
    p->server_req = NULL;
    p->dont_free = true;
    proxy_cancel_direct(p);
    proxy_cancel_proxy(p);
    p->dont_free = false;
    proxy_request_cleanup(p);
}

void submit_request(network *n, evhttp_request *server_req)
{
    proxy_request *p = alloc(proxy_request);
    p->n = n;
    p->start_time = us_clock();
    TAILQ_INIT(&p->direct_headers);
    p->cache_file = -1;
    p->server_req = server_req;
    evhttp_connection_set_closecb(p->server_req->evcon, server_evcon_close_cb, p);

    // https://github.com/libevent/libevent/issues/510
    int fd = bufferevent_getfd(evhttp_connection_get_bufferevent(server_req->evcon));
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    getpeername(fd, (sockaddr *)&ss, &len);
    // TODO: we could request directly if we then fetched the signature
    if (!NO_DIRECT && addr_is_localhost((sockaddr *)&ss, len)) {
        direct_submit_request(p);
    }
    proxy_submit_request(p);
}

typedef struct {
    network *n;
    pending_request r;
    peer_connection *pc;
} trace_request;

void trace_request_cleanup(trace_request *t)
{
    if (t->pc) {
        peer_disconnect(t->pc);
        t->pc = NULL;
    }
    free(t);
}

void trace_error_cb(enum evhttp_request_error error, void *arg)
{
    trace_request *t = (trace_request*)arg;
    debug("t:%p trace_error_cb %d\n", t, error);
    if (error != EVREQ_HTTP_REQUEST_CANCEL && peer_is_injector(t->pc->peer)) {
        injector_reachable = 0;
    }
    trace_request_cleanup(t);
}

void trace_request_done_cb(evhttp_request *req, void *arg)
{
    trace_request *t = (trace_request*)arg;
    debug("t:%p trace_request_done_cb req:%p\n", t, req);
    if (!req) {
        return;
    }
    evbuffer *input = req->input_buffer;
    if (req->response_code != 0) {
        const char *sign = evhttp_find_header(req->input_headers, "X-Sign");
        if (!sign) {
            fprintf(stderr, "no signature on TRACE!\n");
        } else {
            crypto_generichash_state content_state;
            crypto_generichash_init(&content_state, NULL, 0, crypto_generichash_BYTES);
            hash_headers(req->input_headers, &content_state);
            unsigned char *body = evbuffer_pullup(input, evbuffer_get_length(input));
            crypto_generichash_update(&content_state, body, evbuffer_get_length(input));
            uint8_t content_hash[crypto_generichash_BYTES];
            crypto_generichash_final(&content_state, content_hash, sizeof(content_hash));
            debug("verifying sig for TRACE %s %s\n", evhttp_request_get_uri(req), sign);
            if (verify_signature(content_hash, sign)) {
                debug("signature good!\n");
                t->pc->peer->last_connect = time(NULL);
                t->pc->peer->last_verified = time(NULL);
                if (peer_is_injector(t->pc->peer)) {
                    injector_reachable = time(NULL);
                    update_injector_proxy_swarm(t->n);
                }
                peer_reuse(t->n, t->pc);
                t->pc = NULL;
            }
        }
    }
    trace_request_cleanup(t);
}

void trace_submit_request_on_con(trace_request *t, evhttp_connection *evcon)
{
    evhttp_request *req = evhttp_request_new(trace_request_done_cb, t);
    overwrite_header(req, "Proxy-Connection", "Keep-Alive");
    evhttp_request_set_error_cb(req, trace_error_cb);
    char request_uri[256];
    snprintf(request_uri, sizeof(request_uri), "/%u%u%u",
             randombytes_random(), randombytes_random(), randombytes_random());
    evhttp_make_request(evcon, req, EVHTTP_REQ_TRACE, request_uri);
    debug("t:%p con:%p trace request submitted: %s\n", t, req->evcon, request_uri);
}

void submit_trace_request(network *n)
{
    trace_request *t = alloc(trace_request);
    t->n = n;
    queue_request(n, &t->r, ^(peer_connection *pc) {
        t->pc = pc;
        trace_submit_request_on_con(t, t->pc->evcon);
    });
}

typedef struct {
    evhttp_request *server_req;
    evhttp_request *proxy_req;
    bufferevent *direct;
    pending_request r;
    peer_connection *pc;
} connect_req;

void connect_cleanup(connect_req *c)
{
    if (c->direct || c->proxy_req || c->r.connected) {
        return;
    }
    if (c->server_req) {
        if (c->server_req->evcon) {
            evhttp_connection_set_closecb(c->server_req->evcon, NULL, NULL);
        }
        evhttp_send_error(c->server_req, 504, "Gateway Timeout");
        c->server_req = NULL;
    }
    if (c->pc) {
        peer_disconnect(c->pc);
        c->pc = NULL;
    }
    free(c);
}

void connected(connect_req *c, bufferevent *other)
{
    debug("c:%p connected other:%p\n", c, other);
    evhttp_connection *evcon = c->server_req->evcon;
    bufferevent *bev = evhttp_connection_detach_bufferevent(evcon);
    debug("c:%p detach from server evcon:%p bev:%p\n", c, evcon, bev);
    bufferevent_setcb(bev, NULL, NULL, NULL, NULL);
    evhttp_connection_set_closecb(evcon, NULL, NULL);
    c->server_req = NULL;
    connect_cleanup(c);
    evbuffer_add_printf(bufferevent_get_output(bev), "HTTP/1.0 200 Connection established\r\n\r\n");
    bev_splice(bev, other);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
    bufferevent_enable(other, EV_READ|EV_WRITE);
}

void connect_proxy_cancel(connect_req *c)
{
    if (c->proxy_req) {
        evhttp_cancel_request(c->proxy_req);
        c->proxy_req = NULL;
    }
    if (!c->pc) {
        abort_connect(&c->r);
    }
}

void connect_direct_cancel(connect_req *c)
{
    if (c->direct) {
        bufferevent_free(c->direct);
        c->direct = NULL;
    }
}

void connect_done_cb(evhttp_request *req, void *arg)
{
    connect_req *c = (connect_req *)arg;
    if (!req) {
        return;
    }
    c->proxy_req = NULL;
    connect_cleanup(c);
}

int connect_header_cb(evhttp_request *req, void *arg)
{
    connect_req *c = (connect_req *)arg;
    debug("c:%p connect_header_cb %d %s\n", c, req->response_code, req->response_code_line);
    if (req->response_code != 200) {
        return -1;
    }

    c->pc->peer->last_connect = time(NULL);
    c->pc = NULL;

    connect_direct_cancel(c);

    debug("c:%p detach from client evcon:%p\n", c, req->evcon);
    connected(c, evhttp_connection_detach_bufferevent(req->evcon));
    return -1;
}

void connect_error_cb(enum evhttp_request_error error, void *arg)
{
    connect_req *c = (connect_req *)arg;
    debug("c:%p connect_error_cb %d\n", c, error);
    c->proxy_req = NULL;
    assert(!c->r.connected);
    connect_cleanup(c);
}

void connect_event_cb(bufferevent *bev, short events, void *ctx)
{
    connect_req *c = (connect_req *)ctx;
    debug("c:%p connect_event_cb events:0x%x bev:%p req:%s\n", c, events, bev, evhttp_request_get_uri(c->server_req));

    if (events & (BEV_EVENT_EOF|BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT)) {
        bufferevent_free(bev);
        c->direct = NULL;
        connect_cleanup(c);
    } else if (events & BEV_EVENT_CONNECTED) {
        connect_proxy_cancel(c);
        c->direct = NULL;
        connected(c, bev);
    }
}

void connect_evcon_close_cb(evhttp_connection *evcon, void *ctx)
{
    connect_req *c = (connect_req *)ctx;
    debug("c:%p connect_evcon_close_cb\n", c);
    evhttp_connection_set_closecb(evcon, NULL, NULL);
    c->server_req = NULL;
    connect_proxy_cancel(c);
    connect_direct_cancel(c);
    connect_cleanup(c);
}

void connect_request(network *n, evhttp_request *req)
{
    char buf[2048];
    snprintf(buf, sizeof(buf), "https://%s", evhttp_request_get_uri(req));
    evhttp_uri *uri = evhttp_uri_parse(buf);
    const char *host = evhttp_uri_get_host(uri);
    if (!host) {
        evhttp_uri_free(uri);
        evhttp_send_error(req, 400, "Invalid Host");
        return;
    }
    int port = evhttp_uri_get_port(uri);
    if (port == -1) {
        port = 443;
    } else if (port != 443) {
        evhttp_uri_free(uri);
        evhttp_send_error(req, 403, "Port is not 443");
        return;
    }

    connect_req *c = alloc(connect_req);
    c->server_req = req;

    evhttp_connection_set_closecb(c->server_req->evcon, connect_evcon_close_cb, c);

    queue_request(n, &c->r, ^(peer_connection *pc) {
        c->pc = pc;
        c->proxy_req = evhttp_request_new(connect_done_cb, c);

        append_via(c->server_req, c->proxy_req);

        evhttp_request_set_header_cb(c->proxy_req, connect_header_cb);
        evhttp_request_set_error_cb(c->proxy_req, connect_error_cb);
        evhttp_make_request(c->pc->evcon, c->proxy_req, EVHTTP_REQ_CONNECT, evhttp_request_get_uri(c->server_req));
    });

#if !NO_DIRECT
    c->direct = bufferevent_socket_new(n->evbase, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(c->direct, NULL, NULL, connect_event_cb, c);
    bufferevent_enable(c->direct, EV_READ);
    bufferevent_socket_connect_hostname(c->direct, n->evdns, AF_INET, host, port);
    evhttp_uri_free(uri);
#endif
}

int evhttp_parse_firstline_(evhttp_request *, evbuffer*);
int evhttp_parse_headers_(evhttp_request *, evbuffer*);

void http_request_cb(evhttp_request *req, void *arg)
{
    network *n = (network*)arg;
    char *e_host;
    ev_uint16_t e_port;
    evhttp_connection_get_peer(req->evcon, &e_host, &e_port);
    debug("req:%p evcon:%p %s:%u received %s %s\n", req, req->evcon, e_host, e_port,
        evhttp_method(req->type), evhttp_request_get_uri(req));

    connect_more_injectors(n);

    const char *via = evhttp_find_header(req->input_headers, "Via");
    if (via && strstr(via, via_tag)) {
        evhttp_send_error(req, 403, "Via Loop");
    }

    if (req->type == EVHTTP_REQ_CONNECT) {
        connect_request(n, req);
        return;
    }

    char *encoded_uri = cache_name_from_uri(evhttp_request_get_uri(req));
    char cache_path[PATH_MAX];
    char cache_headers_path[PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s%s", CACHE_PATH, encoded_uri);
    snprintf(cache_headers_path, sizeof(cache_headers_path), "%s.headers", cache_path);
    free(encoded_uri);
    int cache_file = open(cache_path, O_RDONLY);
    int headers_file = open(cache_headers_path, O_RDONLY);
    debug("check hit:%d,%d cache:%s\n", cache_file != -1, headers_file != -1, cache_path);
    if (!NO_CACHE && cache_file != -1 && headers_file != -1) {
        evhttp_request *temp = evhttp_request_new(NULL, NULL);
        evbuffer *header_buf = evbuffer_new();
        off_t length = lseek(headers_file, 0, SEEK_END);
        evbuffer_add_file(header_buf, headers_file, 0, length);
        evhttp_parse_firstline_(temp, header_buf);
        evhttp_parse_headers_(temp, header_buf);
        copy_response_headers(temp, req);
        evbuffer_free(header_buf);

        evbuffer *content = NULL;
        length = lseek(cache_file, 0, SEEK_END);

        const char *ifnonematch = evhttp_find_header(req->input_headers, "If-None-Match");
        if (ifnonematch) {
            const char *sign = evhttp_find_header(temp->output_headers, "X-Sign");
            size_t out_len = 0;
            uint8_t *content_hash = base64_decode(ifnonematch, strlen(ifnonematch), &out_len);
            if (out_len == crypto_generichash_BYTES && verify_signature(content_hash, sign)) {
                temp->response_code = 304;
                free(temp->response_code_line);
                temp->response_code_line = strdup("Not Modified");
                close(cache_file);
                cache_file = -1;
            }
            free(content_hash);
        }

        if (cache_file != -1) {
            content = evbuffer_new();
            evbuffer_add_file(content, cache_file, 0, length);
        }
        debug("responding with %d %s length:%u\n", temp->response_code, temp->response_code_line, length);
        evhttp_send_reply(req, temp->response_code, temp->response_code_line, content);
        evhttp_request_free(temp);
        if (content) {
            evbuffer_free(content);
        }
        return;
    }
    close(cache_file);
    close(headers_file);

    submit_request(n, req);
}

network* client_init(port_t port)
{
    //o_debug = 0;

    injectors = alloc(peer_array);
    injector_proxies = alloc(peer_array);
    all_peers = alloc(peer_array);
    TAILQ_INIT(&pending_requests);

    // "1.1 _.dcdn"
    via_tag[4] = 'a' + randombytes_uniform(26);

    network *n = network_setup("0.0.0.0", 0);

    utp_set_callback(n->utp, UTP_ON_ACCEPT, &utp_on_accept);

    evhttp_set_allowed_methods(n->http, EVHTTP_REQ_GET | EVHTTP_REQ_HEAD | EVHTTP_REQ_CONNECT | EVHTTP_REQ_TRACE);
    evhttp_set_gencb(n->http, http_request_cb, n);
    evhttp_bind_socket_with_handle(n->http, "127.0.0.1", port);
    printf("listening on TCP:%s:%d\n", "127.0.0.1", port);

    timer_callback cb = ^{
        dht_get_peers(n->dht, (const uint8_t *)injector_swarm);
        submit_trace_request(n);
        update_injector_proxy_swarm(n);
    };
    cb();
    timer_repeating(n, 25 * 60 * 1000, cb);

    return n;
}

int client_run(network *n)
{
    return network_loop(n);
}

int main(int argc, char *argv[])
{
    char *port_s = "8006";

    for (;;) {
        int c = getopt(argc, argv, "p:v");
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'p':
            port_s = optarg;
            break;
        case 'v':
            o_debug = 1;
            break;
        default:
            die("Unhandled argument: %c\n", c);
        }
    }

    network *n = client_init(atoi(port_s));
    return client_run(n);
}
