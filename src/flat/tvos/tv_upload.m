// tv.h's upload: the ROM set from a phone or computer on the same network. Apple TV has no document picker and no
// Files app, so while the app asks for the set (main_tvos.m shows this address and its QR code) it serves a small page
// on the LAN; the page sends the chosen file back in one POST of the raw bytes (no multipart to parse), with a
// progress bar. As GoldenEye VR's Apple TV build takes its ROM (its docs/apple-tv.md).
//
// The file lands in arcade/upload.part and becomes hotd.zip or hotd.7z by its first bytes (a ZIP or a 7z, whatever
// it is called); the other one goes (the game takes hotd.zip first: one set only). One connection at a time on one
// thread: a page, then its POST. Test it from the Mac with the Simulator's app running:
//   curl --data-binary @hotd.7z http://127.0.0.1:8007/upload
#import <Foundation/Foundation.h>
#include "tv.h"
#include "platform/platform.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PORT = 8007, IDLE_S = 30 };
static const long long MAX_SIZE = 1LL << 30;   // (MAME's hotd set is about 40 MB)

static struct {
    int fd, port;
    atomic_int stop, running;
    char dir[1024];
    tv_upload_done done;
    atomic_llong got, total;
    char error[200];
    pthread_t thread;
} U = { .fd = -1 };

static int send_all(int c, const char *p, size_t n) {
    while (n) {
        const ssize_t k = send(c, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}
static void reply(int c, const char *status, const char *type, const char *body) {
    char head[256];
    const int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, strlen(body));
    if (n > 0 && send_all(c, head, (size_t)n)) send_all(c, body, strlen(body));
}

static const char PAGE[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>House of the Dead VR on Apple TV</title><style>"
    ":root{color-scheme:dark}body{margin:0;background:#0c131d;color:#eef3f6;font:17px/1.45 -apple-system,system-ui,\"Segoe UI\",sans-serif}"
    "main{max-width:34em;margin:0 auto;padding:28px 16px}h1{color:#ffca6e;font-size:1.35em;margin:0 0 .4em}"
    "label{display:inline-block;margin:.6em 0;padding:.7em 1.2em;border-radius:10px;background:#36b7cf;color:#0c131d;font-weight:600;cursor:pointer}"
    "input{display:none}#bar{height:10px;border-radius:5px;background:#1d2a3a;overflow:hidden;margin:1em 0 .4em}"
    "#fill{height:100%;width:0;background:#36b7cf}small{color:#9aa7b4}</style></head><body><main>"
    "<h1>House of the Dead VR on Apple TV</h1>"
    "<p>Send the game's ROM set to your Apple TV: MAME's <b>hotd</b> set (Revision A), as <b>hotd.zip</b> or <b>hotd.7z</b>.</p>"
    "<label>Choose the file and send it<input type=\"file\" id=\"file\" accept=\".zip,.7z\"></label>"
    "<div id=\"bar\"><div id=\"fill\"></div></div><p id=\"msg\"></p>"
    "<p><small>This page works while the TV shows its address. The file goes straight to the Apple TV on your network, nowhere else.</small></p>"
    "</main><script>"
    "var file=document.getElementById('file'),fill=document.getElementById('fill'),msg=document.getElementById('msg');"
    "file.onchange=function(){var f=file.files[0];if(!f)return;var x=new XMLHttpRequest();"
    "x.open('POST','/upload?name='+encodeURIComponent(f.name));"
    "x.upload.onprogress=function(e){if(!e.lengthComputable)return;var p=Math.floor(100*e.loaded/e.total);fill.style.width=p+'%';msg.textContent='Sending '+f.name+': '+p+'%';};"
    "x.onload=function(){msg.textContent=x.responseText;};"
    "x.onerror=function(){msg.textContent='Sending failed. Is the TV still showing its address?';};"
    "msg.textContent='Sending '+f.name+'...';x.send(f);file.value='';};"
    "</script></body></html>";

// waits for bytes a second at a time (a stop is noticed); <= 0: the sender closed, failed or went quiet
static ssize_t recv_some(int c, char *buf, size_t n) {
    for (int idle = 0; idle < IDLE_S && !atomic_load(&U.stop);) {
        const ssize_t k = recv(c, buf, n, 0);
        if (k >= 0) return k;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        idle++;
    }
    return -1;
}

static void finished(const char *path) {
    char *p = strdup(path);
    tv_upload_done cb = U.done;
    dispatch_async(dispatch_get_main_queue(), ^{ if (cb) cb(p); free(p); });
}

static void receive(int c, long long length, const char *start, size_t have) {
    if (length <= 0) { reply(c, "411 Length Required", "text/plain; charset=utf-8", "No file arrived.\n"); return; }
    if (length > MAX_SIZE) { reply(c, "413 Payload Too Large", "text/plain; charset=utf-8", "That file is far too large for the ROM set.\n"); return; }
    char part[1100], dst[1100], other[1100];
    snprintf(part, sizeof part, "%s/upload.part", U.dir);
    FILE *out = fopen(part, "wb");
    if (!out) { reply(c, "500 Internal Server Error", "text/plain; charset=utf-8", "The Apple TV could not store the file.\n"); return; }
    atomic_store(&U.total, length);
    atomic_store(&U.got, 0);
    U.error[0] = 0;
    unsigned char magic[6] = { 0 };
    long long got = 0;
    int ok = 1;
    static char buf[1 << 16];
    for (;;) {
        size_t n = have;
        const char *p = start;
        if (!n) {
            if (got >= length) break;
            const ssize_t k = recv_some(c, buf, sizeof buf);
            if (k <= 0) { ok = 0; break; }
            n = (size_t)k; p = buf;
        }
        have = 0;
        if (got + (long long)n > length) n = (size_t)(length - got);
        for (size_t i = 0; i < n && got + (long long)i < 6; i++) magic[got + i] = (unsigned char)p[i];
        if (n && fwrite(p, 1, n, out) != n) { ok = 0; break; }
        got += (long long)n;
        atomic_store(&U.got, got);
        if (got >= length) break;
    }
    if (fclose(out) != 0) ok = 0;
    atomic_store(&U.total, 0);
    const int zip = !memcmp(magic, "PK\3\4", 4), sz = !memcmp(magic, "7z\xBC\xAF\x27\x1C", 6);
    if (!ok || got < length) {
        remove(part);
        plat_log(PLAT_LOG_ERROR, "tv: the ROM set stopped after %lld of %lld bytes", got, length);
        reply(c, "500 Internal Server Error", "text/plain; charset=utf-8", "The file did not arrive whole. Try again.\n");
        return;
    }
    if (!zip && !sz) {
        remove(part);
        snprintf(U.error, sizeof U.error, "That file was not a ZIP or 7z. Send hotd.zip or hotd.7z.");
        plat_log(PLAT_LOG_ERROR, "tv: a file that is not a ZIP or 7z (%lld bytes) refused", got);
        reply(c, "415 Unsupported Media Type", "text/plain; charset=utf-8", "That is not a ZIP or 7z. Send MAME's hotd set: hotd.zip or hotd.7z.\n");
        return;
    }
    snprintf(dst, sizeof dst, "%s/%s", U.dir, zip ? "hotd.zip" : "hotd.7z");
    snprintf(other, sizeof other, "%s/%s", U.dir, zip ? "hotd.7z" : "hotd.zip");
    remove(other);
    if (rename(part, dst)) {
        remove(part);
        reply(c, "500 Internal Server Error", "text/plain; charset=utf-8", "The Apple TV could not store the file.\n");
        return;
    }
    plat_log(PLAT_LOG_INFO, "tv: the ROM set arrived, %lld bytes (%s)", got, zip ? "hotd.zip" : "hotd.7z");
    reply(c, "200 OK", "text/plain; charset=utf-8", "Done: the Apple TV has the ROM set. Look at the TV.\n");
    finished(dst);
}

static void serve(int c) {
    const int one = 1;
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    struct timeval second = { 1, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &second, sizeof second);
    static char head[16384];
    size_t n = 0;
    char *end = NULL;
    while (!end && n < sizeof head - 1) {
        const ssize_t k = recv_some(c, head + n, sizeof head - 1 - n);
        if (k <= 0) return;
        n += (size_t)k;
        head[n] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    if (!end) { reply(c, "431 Request Header Fields Too Large", "text/plain", "Too large.\n"); return; }
    const char *body = end + 4;
    const size_t have = n - (size_t)(body - head);
    *end = 0;
    if (!strncmp(head, "GET / ", 6) || !strncmp(head, "GET /index.html ", 16)) { reply(c, "200 OK", "text/html; charset=utf-8", PAGE); return; }
    if (!strncmp(head, "POST /upload", 12)) {
        long long length = -1;
        for (char *l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n"))
            if (!strncasecmp(l + 2, "Content-Length:", 15)) length = atoll(l + 17);
        receive(c, length, body, have);
        return;
    }
    reply(c, "404 Not Found", "text/plain", "Not here.\n");
}

static void *listener(void *arg) {
    (void)arg;
    while (!atomic_load(&U.stop)) {
        struct pollfd p = { U.fd, POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0) continue;
        const int c = accept(U.fd, NULL, NULL);
        if (c < 0) continue;
        serve(c);
        close(c);
    }
    return NULL;
}

int tv_upload_start(const char *arcade_dir, tv_upload_done done) {
    if (atomic_load(&U.running)) return U.port;
    snprintf(U.dir, sizeof U.dir, "%s", arcade_dir);
    U.done = done;
    U.error[0] = 0;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(PORT);
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) {   // taken: any free port
        a.sin_port = 0;
        if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return 0; }
    }
    socklen_t len = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &len);
    if (listen(fd, 4)) { close(fd); return 0; }
    U.fd = fd;
    U.port = ntohs(a.sin_port);
    atomic_store(&U.stop, 0);
    if (pthread_create(&U.thread, NULL, listener, NULL)) { close(fd); U.fd = -1; return 0; }
    atomic_store(&U.running, 1);
    plat_log(PLAT_LOG_INFO, "tv: the ROM upload page on port %d", U.port);
    return U.port;
}

void tv_upload_stop(void) {
    if (!atomic_load(&U.running)) return;
    atomic_store(&U.stop, 1);
    pthread_join(U.thread, NULL);
    close(U.fd);
    U.fd = -1;
    atomic_store(&U.running, 0);
    plat_log(PLAT_LOG_INFO, "tv: the ROM upload page stopped");
}

int tv_upload_url(char *url, int n) {
    struct ifaddrs *all = NULL, *best = NULL;
    url[0] = 0;
    if (getifaddrs(&all)) return 0;
    // Wi-Fi or Ethernet (en*) first; any other up, non-loopback IPv4 otherwise
    for (struct ifaddrs *i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!best || (strncmp(best->ifa_name, "en", 2) && !strncmp(i->ifa_name, "en", 2))) best = i;
    }
    if (best) {
        char ip[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)best->ifa_addr)->sin_addr, ip, sizeof ip);
        snprintf(url, (size_t)n, "http://%s:%d/", ip, U.port);
    }
    freeifaddrs(all);
    return url[0] != 0;
}

int tv_upload_progress(long long *got, long long *total) {
    *got = atomic_load(&U.got);
    *total = atomic_load(&U.total);
    return *total > 0;
}
const char *tv_upload_error(void) { return U.error; }
