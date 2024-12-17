
A primitive TCP/IP based message exchange server and client.

A toy project to investigate the following:

  * Using epoll(7) to wait for events (available input, peer closing
    connection, client connecting).

  * Using the OS facilities (namely getaddrinfo(3)) to resolve host
    and protocol names as well as choosing between IP and IPv6.

  * Investigate failure modes.

  * Using different buffer sizes on client and server side.

  * Play with IPv6.

The intent is educational, not to surpass netcat(1) or telnet(1).


Compile
=======

    $ make


Examples
========

Local communication
-------------------

Run server and client in different terminals.  I tend to choose port
42000.  An interface on `localhost` is chosen by default.

    $ ./tcp -s 42000    # run server
    $ ./tcp 42000       # run client

This establishes a bidirectional channel between stdin of the one
process and stdout of the other, and vice versa.

The server must be started first, and only accepts connections
sequentially (otherwise its stdin would have to be multiplexed to all
clients).


Resolve host and service by name
--------------------------------

Google still provides an unencrypted HTTP server which is patient
enough to let you do the typing:

    $ tcp http www.google.com       # resolves port and IP address
    GET / HTTP/1.1

Add an empty line after typing the `GET` request, this ends the HTTP
request header.


IP or IPv6?
-----------

On my system `tcp` first tries (because getaddrinfo(3) first suggests)
to bind to `::1`, and if that fails (maybe because the address is
alredy in use) tries `127.0.0.1`.

    $ tcp -s 42000 localhost
    tcp: Bound socket 3 to ::1

Leave that server running, and start another one in another terminal:

    $ tcp -s 42000 localhost
    tcp: bind(3, ::1): Address already in use
    tcp: Bound socket 3 to 127.0.0.1

A client will do the same, unless the address is specified
numerically:

    $ tcp 42000 localhost < <(date)       # automatic
    tcp: Connected to ::1

    $ tcp 42000 ::1 < <(date)             # explicit IPv6
    tcp: Connected to ::1

    $ tcp 42000 127.0.0.1 < <(date)       # explicit IP
    tcp: Connected to 127.0.0.1

And even that works:

    # ip addr  add 2001:8d8:1800:419::2  dev lo  valid_lft 30  preferred_lft 30

    $ tcp -s 42666 2001:8d8:1800:419::2
    tcp: Bound socket 3 to 2001:8d8:1800:419::2
    tcp: Connected from 2001:8d8:1800:419::
    sdfsdfsd

    $ tcp 42666 2001:8d8:1800:419::2
    tcp: Connected to 2001:8d8:1800:419::2
    sdfsdfsd

Yay =)
