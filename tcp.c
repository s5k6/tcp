#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>



// parse an unsigned long with a suffix

struct suffix { const char *suf; const unsigned long int val; };

struct suffix volume[] = {
    { .suf = "",   .val = 1 },
    { .suf = "k",  .val = 1000 },
    { .suf = "ki", .val = 1024 },
    { .suf = "M",  .val = 1000000 },
    { .suf = "Mi", .val = 1048576 },
    { .suf = "G",  .val = 1000000000 },
    { .suf = "Gi", .val = 1073741824 },
    { .suf = NULL,  .val = 0 },
};

unsigned long int suffixed(const char *arg, const struct suffix *suffix) {

    char *end;
    unsigned long val = strtoul(arg, &end, 10);

    if (end == arg)
        errx(1, "No digits in `%s`", arg);

    for (int i = 0; suffix[i].suf; i++)
        if (strcmp(end, suffix[i].suf) == 0)
            return val * suffix[i].val;

    errx(1, "Invalid unit: `%s` following `%ld`", end, val);
}



// one global variable capturing the configuration

static struct {
    int serverRole;
    int quitAfter;
    const char *service;
    const char *host;
    size_t bufSize;
} cfg = {
    .serverRole = 0,
    .quitAfter = 0,
    .service = NULL,
    .host = "localhost",
    .bufSize = 1024
};




// representation of socket address

const char *sockaddr2string(
    const struct sockaddr *sa, char *dst, socklen_t lim, in_port_t *port
) {

    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)sa;
        *port = ntohs(sin->sin_port);
        return inet_ntop(AF_INET, &(sin->sin_addr), dst, lim);
    }

    if (sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;
        *port = ntohs(sin6->sin6_port);
        return inet_ntop(AF_INET6, &(sin6->sin6_addr), dst, lim);
    }

    return NULL;
}



// Signal handler: Store signal number.

static int sig = 0; // Last signal received

static void handler(int s) {
    sig = s;
}



/* Copy bytes from one file descriptor to another.  Read into the
passed buffer once, then repeat writing until all is passed.  Thus,
this may block when writing blocks.  Being called only upon an epoll
event, reading should never block. */

ssize_t transfer(int from, int to, char *buf) {

    ssize_t readBytes = read(from, buf, cfg.bufSize);
    if (readBytes < 0)
        err(1, "read(%d)", from);

    for (ssize_t m = 0, q; m < readBytes; m += q) {
        size_t rest = (size_t)(readBytes - m);
        q = write(to, buf + m, rest);
        if (q < 0)
            err(1, "write(%d)", to);
    }

    return readBytes;
}



/* Add pre-populated epoll_event struct with new file descriptor f,
and handle errors. */

#define epoll_add(EFD, EVENT, FD)                                       \
    do {                                                                \
        (EVENT).data.fd = (FD);                                         \
        if (epoll_ctl(EFD, EPOLL_CTL_ADD, (FD), &(EVENT)) == -1)        \
            err(1, "epoll_ctl(%d, %d)", EFD, (EVENT).data.fd);          \
    } while(0)



// communication over established connectio

void communicate(int conn) {

    // configure epoll for input events
    int epollfd;
    {
        epollfd = epoll_create1(EPOLL_CLOEXEC);
        if (epollfd == -1)
            err(1, "epoll_create1");

        struct epoll_event ev;
        ev.events = EPOLLIN;
        epoll_add(epollfd, ev, 0);
        epoll_add(epollfd, ev, conn);
    }


    // communicate

    char *buf = malloc(cfg.bufSize);
    if (buf == NULL)
        err(1, "malloc(%zu)", cfg.bufSize);

    int run = 1;
    do {

        const int maxEvents = 10;
        struct epoll_event events[maxEvents];

        // -1 → no timeout
        ssize_t eventCount = epoll_wait(epollfd, events, maxEvents, -1);
        if (eventCount < 0)
            if (errno != EINTR)
                err(1, "epoll_wait(%d)", epollfd);

        for (ssize_t i = 0; i < eventCount; i++) {
            if (events[i].data.fd == 0) { // stdin
                if (transfer(0, conn, buf) < 1)
                    run = 0;
            } else if (events[i].data.fd == conn) {
                if (transfer(conn, 1, buf) < 1)
                    run = 0;
            } else {
                errx(1, "unexpected event");
            }
        } // iterating over events

    } while (sig == 0 && run);

    free(buf);

    if (sig)
        warnx("Communicating loop caught signal %d", sig);

    if (close(conn))
        warn("close(%d)", conn);

}



void serve(const struct addrinfo *result) {

    int sock;

    const struct addrinfo *rp;
    for (rp = result; rp; rp = rp->ai_next) {

        char text[512];
        in_port_t port;
        if (!sockaddr2string(rp->ai_addr, text, sizeof(text), &port))
            err(1, "sockaddr2string");

        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            warn("socket(%s port %d)", text, port);
            continue;
        }

        int opt;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
            err(1, "setsockopt(%d)", sock);

        if (bind(sock, rp->ai_addr, rp->ai_addrlen))
            warn("bind(%d, %s port %d)", sock, text, port);
        else {
            warnx("Bound socket %d to %s port %d", sock, text, port);
            if (listen(sock, 0))
                warn("listen(%d)", sock);
            else
                break;  // success, stop trying
        }
        close(sock); // was invalid
    }

    if (!rp)
        errx(1, "Could not bind");

    // configure epoll for multiplexing accept and discard stdin
    int epollfd;
    {
        epollfd = epoll_create1(EPOLL_CLOEXEC);
        if (epollfd == -1)
            err(1, "epoll_create1");

        struct epoll_event ev;
        ev.events = EPOLLIN;
        epoll_add(epollfd, ev, 0);
        epoll_add(epollfd, ev, sock);
    }

    // this is the server loop, handling clients one by one
    int run = 1;
    do {
        warnx("Waiting for connection...");

        const int maxEvents = 10;
        struct epoll_event events[maxEvents];

        // -1 → no timeout
        ssize_t eventCount = epoll_wait(epollfd, events, maxEvents, -1);
        if (eventCount < 0)
            if (errno != EINTR)
                err(1, "epoll_wait(%d)", epollfd);


        for (ssize_t i = 0; i < eventCount; i++) {

            if (events[i].data.fd == 0) { // discard stdin
                char *buf[512];
                ssize_t c = read(0, buf, sizeof(buf));
                if (c < 0)
                    err(1, "read(0)");
                warnx("Discard %zu bytes", c);

            } else if (events[i].data.fd == sock) {

                struct sockaddr peer;
                socklen_t peerLen = sizeof(peer);
                int conn = accept(sock, &peer, &peerLen);
                if (conn < 0)
                    warn("accept(%d)", sock);
                else {
                    char remote[512];
                    in_port_t port;
                    if (!sockaddr2string(
                            &peer, remote, sizeof(remote), &port
                        ))
                        err(1, "sockaddr2string");
                    warnx("Connected from %s port %d", remote, port);

                    communicate(conn);  // will close conn socket

                    if (cfg.quitAfter)
                        run = 0;
                }

            } else {
                errx(1, "unexpected event");
            }

        } // iterating over events

    } while (sig == 0 && run);

    if (sig)
        warnx("Accepting loop caught signal %d", sig);

    close(sock);

}



void consume(const struct addrinfo *result) { // client role

    int sock;

    const struct addrinfo *rp;
    for (rp = result; rp; rp = rp->ai_next) {

        char text[512];
        in_port_t port;
        if (!sockaddr2string(rp->ai_addr, text, sizeof(text), &port))
            err(1, "sockaddr2string");

        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            warn("socket(%s port %d)", text, port);
            continue;
        }

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) > -1) {
            warnx("Connected to %s port %d", text, port);
            break;
        }
        warn("connect(%s port %d)", text, port);
        close(sock); // was invalid
    }

    if (!rp)
        errx(1, "Could not connect");

    communicate(sock);  // will close socket

}



int main(int argc, char **argv) {

    // no buffering on stdout, must be set before doing any output.
    setbuf(stdout, NULL);



    // CLI arguments

    // help if wrong number of arguments
    if (argc < 2 || 5 < argc) {
        printf("\n%s\n",
#include "help.inc"
               );
        return 0;
    }

    // parse CLI
    {
        int parc = 0;
        char *parv[argc];

        // find optional flags amongst (ordered) parameters
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-')
                if (strcmp("-s", argv[i]) == 0)
                    cfg.serverRole = 1;
                else if (strcmp("-q", argv[i]) == 0)
                    cfg.quitAfter = 1;
                else if (strncmp("-b", argv[i], 2) == 0)
                    cfg.bufSize = (size_t)suffixed(argv[i]+2, volume);
                else
                    errx(1, "Unknown flag: %s", argv[i]);
            else
                parv[parc++] = argv[i];
        }
        cfg.serverRole = cfg.serverRole || cfg.quitAfter;

        if (parc < 1)
            errx(1, "Run without arguments for help.");
        cfg.service = parv[0];

        if (parc > 1)
            cfg.host = parv[1];

        warnx(
            "pid %d, mode %s, service %s, address %s, buf %zu",
            getpid(),
            cfg.serverRole ? (cfg.quitAfter ? "serve once" : "serve many")
                : "client",
            cfg.service, cfg.host, cfg.bufSize
        );

    }



    /* Set up signal handlers: SIGINT, usually issued by pressing C-c.
    SIGPIPE, because we want to produce error messages instead of
    being killed. */
    {
        int handle[] = { SIGINT, SIGPIPE, 0 };

        struct sigaction action;
        memset(&action, 0, sizeof(action));
        if (sigfillset(&action.sa_mask) < 0)
            err(1, "sigfillset");
        action.sa_handler = handler;

        for (int i = 0; handle[i]; i++)
            if (sigaction(handle[i], &action, NULL))
                err(1, "sigaction(%d)", handle[i]);
    }



    /* Get the `result` set of address info records.  The will be
    tried in turn until one can be used to listen (server) or to
    connect (client). */

    struct addrinfo *result;
    {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        int s = getaddrinfo(cfg.host, cfg.service, &hints, &result);
        if (s) // getaddrinfo does not use errno
            errx(
                1,
                "getaddrinfo(%s, %s): %s",
                cfg.host,
                cfg.service,
                gai_strerror(s)
            );
    }

    if (cfg.serverRole)
        serve(result);
    else
        consume(result);

    freeaddrinfo(result);

    return 0;
}


/* Notes are found in README.md */
