
TCP/IP server and client
========================

A toy project to investigate the following:

  * Using epoll(7) to wait for events (available input, peer closing
    connection, client connecting).

  * Using the OS facilities (namely getaddrinfo(3)) to resolve host
    and protocol names as well as choosing between IP and IPv6.

  * Investigate failure modes and half-closed connections.

  * Using different buffer sizes on client and server side.

  * Play with IPv6.

The intent is educational, not to surpass netcat(1) or telnet(1).


Compile
-------

    $ make


Examples
========

`tcp` prints a help text when invoked without arguments.


Local communication
-------------------

Run server and client in different terminals.  I tend to choose port
42000.  An interface on `localhost` is chosen by default.

    $ ./tcp 42000 -s    # run server
    $ ./tcp 42000       # run client

This establishes a bidirectional channel between stdin of the one
process and stdout of the other, and vice versa.

The server must be started first, and only accepts connections
sequentially.

Both, server and client, must shut down their writing end of the
connection.  To have this done automatically, add `-q` to the command.


Resolve host and service by name
--------------------------------

Google still provides an unencrypted HTTP server which is patient
enough to let you do the typing:

    $ tcp http www.google.com       # resolves port and IP address
    GET / HTTP/1.1

Add an empty line after typing the `GET` request, this ends the HTTP
request header.


Running a process
-----------------

The server can fork a process with its standard IO streams replaced by
the socket.  When operated this way, it will handle multiple concurrent
clients by spawning additional processes.

    $ tcp 1234 -s -- rev
    $ tcp 1234

The child process determines how it wants to do buffering, and when it
closes its streams.  Above invocation accumulates multiple lines
before sending a response, and only send the final response on
terminating.

If you want line buffering, modify the serving process:

    $ tcp 1234 -s -- stdbuf -oL rev
    $ tcp 1234 -q

Note the use of `-q`, which makes the client terminate when the
server's child process dies.  Try without `-q` on the client side, and
then kill the process handling the connection on the server side — the
client will wait for the other side to shut down the connection.

The need for `-q` depends on the use case, e.g., a server process may
want to send data when the client closes its sending end of the
connection, as above for the not line-buffered invocation of `rev`.
In that case, the client would terminate too early with `-q`:

    $ tcp 1234 -s -- rev
    $ tcp 1234 -q             # bad

A command can also be used when `tcp` is run as client.  The command
simply replaces `tcp` after establishing a connection.  The following
sends a string back and forth:

    $ tcp 1234 -s -- sh -c 'while read x; do echo "$x"; sleep 1; done'
    $ tcp 1234 -- sh -c 'date; stdbuf -oL rev | tee /dev/stderr'

Ping pong!


IP or IPv6?
-----------

On my system `tcp` first tries (because getaddrinfo(3) first suggests)
to bind to `::1`, and if that fails (maybe because the address is
already in use) tries `127.0.0.1`.

    $ tcp -s 42000 -- date
    tcp: Bound socket 3 to ::1

Leave that server running, and start another one in another terminal:

    $ tcp -s 42000 -- date
    tcp: bind(3, ::1): Address already in use
    tcp: Bound socket 3 to 127.0.0.1

A client will do the same, unless the address is specified
numerically:

    $ tcp -q 42000                        # automatic
    tcp: Connected to ::1

    $ tcp -q 42000 localhost              # exactly the same as above
    tcp: Connected to ::1

    $ tcp -q 42000 ::1                    # explicit IPv6
    tcp: Connected to ::1

    $ tcp -q 42000 127.0.0.1              # explicit IP
    tcp: Connected to 127.0.0.1

And even that works:

    # ip addr  add 2001:8d8:1800:419::2  dev lo  \
      valid_lft 300  preferred_lft 300

    $ tcp -s 42666 2001:8d8:1800:419::2 -- date
    tcp: Bound socket 3 to 2001:8d8:1800:419::2
    tcp: Connected from 2001:8d8:1800:419:: port 35254

    $ tcp 42666 2001:8d8:1800:419::2 -q
    tcp: Connected to 2001:8d8:1800:419::2 port 42666

Yay =)

The magic of (port and host) name resolution, and the parsing of
numeric addresses is all due to getaddrinfo(3).
