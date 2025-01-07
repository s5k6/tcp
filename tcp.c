#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


/* Replace `close` */

#define close(fd)                               \
    do {                                        \
        if (close(fd))                          \
            err(1, "close(%d)", (fd));          \
    } while (0)



/*  one global variable capturing the configuration */

static struct {
    int serverRole;
    int allowHalf;
    const char *service;
    const char *host;
    char **cmdv;
    size_t bufSize;
} cfg = {
    .serverRole = 0,
    .allowHalf = 1,
    .service = NULL,
    .host = "localhost",
    .cmdv = NULL,
    .bufSize = 0
};



/*  parse an unsigned long with a suffix */

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



/* Convert network address structure to a character string.  Simply a
frontend to inet_ntop(3) which dispatches between IP and IPv6. */

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



/*  Signal handler: Store signal number. */

static int sig = 0; // Last signal received

static void handler(int s) {
    if (s != SIGCHLD)
        sig = s;
}



/* Copy bytes from one file descriptor to another.  Read into the
passed buffer once, then repeat writing until all is passed.  Thus,
this may block when writing blocks.  Being called only upon an epoll
event, reading should never block. */

ssize_t transfer(int from, int to, char *buf) {

    ssize_t readBytes = read(from, buf, cfg.bufSize);
    if (readBytes < 0)
        err(1, "read(%d, %zu)", from, cfg.bufSize);

    for (ssize_t m = 0, q; m < readBytes; m += q) {
        size_t rest = (size_t)(readBytes - m);
        q = write(to, buf + m, rest);
        if (q < 0)
            err(1, "write(%d, %zd)", to, rest);
    }

    return readBytes;
}



/* Add pre-populated epoll_event struct with new file descriptor f,
and handle errors. */

#define epoll_add(EFD, EVENT, FD)                                       \
    do {                                                                \
        (EVENT).data.fd = (FD);                                         \
        if (epoll_ctl((EFD), EPOLL_CTL_ADD, (FD), &(EVENT)) == -1)      \
            err(1, "epoll_add(%d, %d)", (EFD), (EVENT).data.fd);        \
} while(0)

#define epoll_del(EFD, FD)                                              \
    do {                                                                \
        if (epoll_ctl((EFD), EPOLL_CTL_DEL, (FD), NULL) == -1)          \
            err(1, "epoll_del(%d, %d)", (EFD), (FD));                   \
    } while(0)



/*  communication over established connections */

void communicate(int conn) {

    /*  configure epoll for input events */
    int epollfd;
    {
        epollfd = epoll_create1(EPOLL_CLOEXEC);
        if (epollfd == -1)
            err(1, "epoll_create1");

        struct epoll_event ev;
        ev.events = EPOLLIN;
        epoll_add(epollfd, ev, STDIN_FILENO);
        epoll_add(epollfd, ev, conn);
    }


    /*  communicate */

    char *buf = malloc(cfg.bufSize);
    if (buf == NULL)
        err(1, "malloc(%zu)", cfg.bufSize);

    int sending = 1, recving = 1;
    do {

        const int maxEvents = 10;
        struct epoll_event events[maxEvents];

        /*  -1 → no timeout */
        ssize_t eventCount = epoll_wait(epollfd, events, maxEvents, -1);
        if (eventCount < 0)
            if (errno != EINTR)
                err(1, "epoll_wait(%d)", epollfd);

        for (ssize_t i = 0; i < eventCount; i++) {
            if (events[i].data.fd == STDIN_FILENO) { // stdin
                if (transfer(STDIN_FILENO, conn, buf) < 1) {
                    sending = 0;
                    epoll_del(epollfd, STDIN_FILENO);
                    shutdown(STDIN_FILENO, SHUT_RD);
                    shutdown(conn, SHUT_WR);
                    warnx("Shut down send direction.");
                }
            } else if (events[i].data.fd == conn) {
                if (transfer(conn, STDOUT_FILENO, buf) < 1) {
                    recving = 0;
                    epoll_del(epollfd, conn);
                    shutdown(conn, SHUT_RD);
                    shutdown(STDIN_FILENO, SHUT_WR);
                    warnx("Shut down recv direction.");
                }
            } else {
                errx(1, "unexpected event");
            }
        } // iterating over events

    } while (
        sig == 0
        &&
        (cfg.allowHalf ? sending || recving : sending && recving)
    );

    free(buf);

    if (sig)
        warnx("Communicating loop caught signal %d", sig);

}



/* Replace this programm with the configured command after setting
stdin and stdout to the passed socket.  This will not return. */

void execCommand(int conn) {

    if (dup2(conn, STDIN_FILENO) < 0)
        err(1, "dup2(%d, %d)", conn, STDIN_FILENO);

    if (dup2(conn, STDOUT_FILENO) < 0)
        err(1, "dup2(%d, %d)", conn, STDOUT_FILENO);

    close(conn);

    execvp(cfg.cmdv[0], cfg.cmdv);
    err(1, "execvp(%s)", cfg.cmdv[0]);

}



/* Fork a child process running the configured command, wired to the
passed connection. */

void forkCommand(int conn) {

    pid_t pid = fork();
    if (pid < 0)
        err(1, "fork");

    if (pid == 0)
        execCommand(conn);

    // only reached in the parent process
    close(conn);

    warnx("Connection %d delegated to process %u", conn, pid);

}



/* Traverse the addrinfo list and try to bind and listen, until one
succeeds.  The respective socket is returned.  Compare this
line-by-line with `tryToConnect` below. */

int tryToBind(const struct addrinfo *result) {

    int sock = -1;
    char text[512];
    in_port_t port;

    const struct addrinfo *rp;
    for (rp = result; rp; rp = rp->ai_next) {

        if (sock >= 0) { // cleanup potential leftover from previous loop
            close(sock);
            sock = -1;
        }

        if (!sockaddr2string(rp->ai_addr, text, sizeof(text), &port))
            err(1, "sockaddr2string");

        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            warn("socket(%s port %d)", text, port);
            continue;
        }

        {
            int opt = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
                warn("setsockopt(%d)", sock);
                continue;
            }
        }

        /* On exec, do not pass listening socket. */
        {
            int flags = fcntl(sock, F_GETFD);
            if (flags == -1)
                err(1, "fcntl(%d)", sock);
            flags |= FD_CLOEXEC;
            if (fcntl(sock, F_SETFD, flags) == -1)
                err(1, "fcntl(%d)", sock);
        }

        if (bind(sock, rp->ai_addr, rp->ai_addrlen)) {
            warn("bind(%d, %s port %d)", sock, text, port);
            continue;
        }

        if (listen(sock, 0)) {
            warn("listen(%d)", sock);
            continue;
        }

        break;  // success, stop trying
    }

    if (!rp)
        errx(1, "Could not bind");

    warnx("Bound socket %d to %s port %d", sock, text, port);

    return sock;
}



/* Serve clients connecting to the passed socked, which must have been
set up as listening socket. */

void serve(int sock) {

    /*  configure epoll for multiplexing accept and discard stdin */
    int epollfd;
    {
        epollfd = epoll_create1(EPOLL_CLOEXEC);
        if (epollfd == -1)
            err(1, "epoll_create1");

        struct epoll_event ev;
        ev.events = EPOLLIN;
        epoll_add(epollfd, ev, STDIN_FILENO);
        epoll_add(epollfd, ev, sock);
    }

    /*  this is the server loop, handling clients one by one */
    int run = 1;
    do {
        warnx("Waiting for connection...");

        const int maxEvents = 10;
        struct epoll_event events[maxEvents];

        /*  -1 → no timeout */
        ssize_t eventCount = epoll_wait(epollfd, events, maxEvents, -1);
        if (eventCount < 0)
            if (errno != EINTR)
                err(1, "epoll_wait(%d)", epollfd);

        for (ssize_t i = 0; i < eventCount; i++) {

            if (events[i].data.fd == STDIN_FILENO) { // discard stdin
                char *buf[512];
                ssize_t c = read(STDIN_FILENO, buf, sizeof(buf));
                if (c < 0)
                    err(1, "read(%d, %zu)", STDIN_FILENO, sizeof(buf));
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

                    if (cfg.cmdv)
                        forkCommand(conn);
                    else
                        communicate(conn);
                }

            } else {
                errx(1, "unexpected event");
            }

        } // iterating over events

        /*  check for terminated child processes */
        if (cfg.cmdv) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid < 0)
                if (errno != ECHILD)
                    warn("waitpid");
            if (pid > 0) {
                /* Analyse and print exit status. */
                if (WIFEXITED(status))
                    warnx("Child %d returned %d", pid, WEXITSTATUS(status));
                else if (WIFSIGNALED(status))
                    warnx("Child %d caught %d", pid, WTERMSIG(status));
                else
                    warnx("Dunno why child %d terminated", pid);
            }
        }

    } while (sig == 0 && run);

    if (sig)
        warnx("Accepting loop caught signal %d", sig);

}



/* Traverse the addrinfo list and try to connect, until one succeeds.
The respective socket is returned.  Compare this line-by-line with
`tryToBind` above. */

int tryToConnect(const struct addrinfo *result) { // client role

    int sock = -1;
    char text[512];
    in_port_t port;

    const struct addrinfo *rp;
    for (rp = result; rp; rp = rp->ai_next) {

        if (sock >= 0) { // cleanup potential leftover from previous loop
            close(sock);
            sock = -1;
        }

        if (!sockaddr2string(rp->ai_addr, text, sizeof(text), &port))
            err(1, "sockaddr2string");

        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            warn("socket(%s port %d)", text, port);
            continue;
        }

        if (connect(sock, rp->ai_addr, rp->ai_addrlen)) {
            warn("connect(%d, %s port %d)", sock, text, port);
            continue;
        }

        break;  // success, stop trying
    }

    if (!rp)
        errx(1, "Could not connect");

    warnx("Connected socket %d to %s port %d", sock, text, port);

    return sock;
}



/* Act as client on the passed socket.  This must have been connected. */

void consume(int sock) {

    if (cfg.cmdv)
        execCommand(sock);
    else
        communicate(sock);

}



/* Command line interface */

int parseCli(int argc, char **argv) {

    if (argc < 2) {
        printf("\n%s\n",
#include "help.inc"
               );
        return 1; // tell main to return 0
    }


    /* the fixed-order parameters PORT, HOST are collected here */
    int parc = 0;
    const char *parv[argc];

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-')
            if (strcmp("-s", argv[i]) == 0)
                cfg.serverRole = 1;
            else if (strcmp("-q", argv[i]) == 0)
                cfg.allowHalf = 0;
            else if (strncmp("-b", argv[i], 2) == 0)
                cfg.bufSize = (size_t)suffixed(argv[i]+2, volume);
            else if (strcmp("--", argv[i]) == 0) {
                cfg.cmdv = &argv[i+1];
                i = argc;
            } else
                errx(1, "Unknown flag: %s", argv[i]);
        else
            parv[parc++] = argv[i];
    }

    /* collect ordered parameters */
    if (parc < 1)
        errx(1, "Run without arguments for help.");
    cfg.service = parv[0];

    if (parc > 1)
        cfg.host = parv[1];

    /* sanity checks */
    if (cfg.cmdv) {
        if (!cfg.cmdv[0])
            errx(1, "Empty command.");
        if (cfg.bufSize)
            errx(1, "Buffer size (-b) not relevant with command.");
        if (!cfg.allowHalf)
            errx(1, "Forcing full duplex (-q) not relevant with command.");
    } else {
        if (!cfg.bufSize)
            cfg.bufSize = 1024;
    }

    return 0;
}



int main(int argc, char **argv) {

    if (parseCli(argc, argv))
        return 0;


    /* Set up signal handlers: SIGINT, usually issued by pressing C-c.
    SIGPIPE, because we want to produce error messages instead of
    being killed. */
    {
        int handle[] = { SIGINT, SIGPIPE, SIGCHLD, 0 };

        struct sigaction action;
        memset(&action, 0, sizeof(action));
        if (sigfillset(&action.sa_mask) < 0)
            err(1, "sigfillset");
        action.sa_handler = handler;

        for (int i = 0; handle[i]; i++)
            if (sigaction(handle[i], &action, NULL))
                err(1, "sigaction(%d)", handle[i]);
    }



    /* Get the `result` set of address info records.  These will be
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

    int sock = cfg.serverRole ? tryToBind(result) : tryToConnect(result);
    if (sock < 0)
        errx(1, "Failed to create socket");

    freeaddrinfo(result);

    if (cfg.serverRole)
        serve(sock);
    else
        consume(sock);

    warnx("Closing socket %d", sock);
    close(sock);

    return 0;
}


/* Notes are found in README.md */
