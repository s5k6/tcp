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



// representation of socket address

const char *sockaddr2string(
    const struct sockaddr *sa, char *dst, socklen_t lim
) {

    if (sa->sa_family == AF_INET)
        return inet_ntop(
            AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), dst, lim
        );

    if (sa->sa_family == AF_INET6)
        return inet_ntop(
            AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), dst, lim
        );

    return NULL;
}



// Signal handler: Store signal number.

static int sig = 0; // Last signal received

static void handler(int s) {
    sig = s;
}



/* One buffer is enough for everything, so here it is. */

static char *buf = NULL;

static size_t bufSize = 1024;



/* Copy bytes from one file descriptor to another.  Read into the
global buffer once, then repeat writing until all is passed.  Thus,
this may block. */

ssize_t transfer(int from, int to) {

    ssize_t readBytes = read(from, buf, bufSize);
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

#define epoll_add(efd, ev, f)                                           \
    do {                                                                \
        (ev).data.fd = (f);                                             \
        if (epoll_ctl(efd, EPOLL_CTL_ADD, (ev).data.fd, &(ev)) == -1)   \
            err(1, "epoll_ctl(%d, %d)", efd, (ev).data.fd);             \
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
                if (transfer(0, conn) < 1)
                    run = 0;
            } else if (events[i].data.fd == conn) {
                if (transfer(conn, 1) < 1)
                    run = 0;
            } else {
                errx(1, "unexpected event");
            }
        } // iterating over events

    } while (sig == 0 && run);

    if (sig)
        warnx("Communicating loop caught signal %d", sig);


    // cleanup

    if (shutdown(conn, SHUT_RDWR))
        warn("shutdown(%d)", conn);

    if (close(conn))
        warn("close(%d)", conn);

}




int main(int argc, char **argv) {

    // no buffering on stdout, must be set before doing any output.
    setbuf(stdout, NULL);


    // CLI arguments

    int serverRole = 0;
    const char *port;
    const char *host = "localhost";
    bufSize = 1024;

    // help if wrong number of arguments
    if (argc < 2 || 5 < argc) {
        printf("\n%s\n",
#include "help.inc"
               );
        return 0;
    }

    // parse CLI
    {
        int i = 1;

        if (strcmp("-s", argv[i]) == 0) {
            i += 1;
            serverRole = 1;
        }

        port = argv[i++];

        if (argc > i)
            host = argv[i++];

        if (argc > i)
            bufSize = (size_t)suffixed(argv[i], volume);

        // print report about what will be done
        warnx(
            "Will %s %s, port %s, using %zu bytes buffer.",
            serverRole ? "serve on" : "connect to",
            host, port, bufSize
        );
    }


    // allocate the buffer
    buf = malloc(bufSize);
    if (buf == NULL)
        err(1, "malloc(%zu)", bufSize);


    // Set up signal handler for SIGINT, usually issued by pressing C-c.
    {
        int handle[] = { SIGINT, 0 };

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

        int s = getaddrinfo(host, port, &hints, &result);
        if (s) // getaddrinfo does not use errno
            errx(1, "getaddrinfo(%s, %s): %s", host, port, gai_strerror(s));
    }



    if (serverRole) {

        int sock;

        struct addrinfo *rp;
        for (rp = result; rp; rp = rp->ai_next) {

            char text[512];
            if (!sockaddr2string(rp->ai_addr, text, sizeof(text)))
                err(1, "sockaddr2string");

            sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sock < 0) {
                warn("socket(%s)", text);
                continue;
            }

            int opt;
            if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
                err(1, "setsockopt(%d)", sock);

            if (bind(sock, rp->ai_addr, rp->ai_addrlen))
                warn("bind(%d, %s)", sock, text);
            else {
                warnx("Bound socket %d to %s", sock, text);
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
                    ssize_t c = read(0, buf, bufSize);
                    if (c < 0)
                        err(1, "read(0)");

                } else if (events[i].data.fd == sock) {

                    struct sockaddr peer;
                    socklen_t peerLen = sizeof(peer);
                    int conn = accept(sock, &peer, &peerLen);
                    if (conn < 0)
                        warn("accept(%d)", sock);
                    else {
                        char remote[512];
                        if (!sockaddr2string(&peer, remote, sizeof(remote)))
                            err(1, "sockaddr2string");
                        warnx("Connected from %s", remote);

                        communicate(conn);  // will close conn socket
                    }

                } else {
                    errx(1, "unexpected event");
                }

            } // iterating over events

        } while (sig == 0);
        warnx("Accepting loop caught signal %d", sig);

        close(sock);

    } else { // client role

        int sock;

        struct addrinfo *rp;
        for (rp = result; rp; rp = rp->ai_next) {

            char text[512];
            if (!sockaddr2string(rp->ai_addr, text, sizeof(text)))
                err(1, "sockaddr2string");

            sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sock < 0) {
                warn("socket(%s)", text);
                continue;
            }

            if (connect(sock, rp->ai_addr, rp->ai_addrlen) > -1) {
                warnx("Connected to %s", text);
                break;
            }
            warn("connect(%s)", text);
            close(sock); // was invalid
        }

        if (!rp)
            errx(1, "Could not connect");

        communicate(sock);  // will close socket

    }


    // pointless at end of program
    freeaddrinfo(result);
    free(buf);

    return 0;
}


/* Notes are found in README.md */
