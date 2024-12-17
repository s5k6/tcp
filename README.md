

Compile
-------

    $ make


Example
-------

Run server and client in different terminals.  I tend to choose port
42000, the IP address will always be 127.0.0.1 on loopback.

    $ ./tcp srv localhost 42000 1kB
    $ ./tcp cli localhost 42000 1kB

This establishes a bidirectional channel between stdin of the one
process and stdout of the other, and vice versa.

The server must be started first, and only accepts connections
sequentially (otherwise its stdin would have to be multiplexed to all
clients).

They use the epoll interface to select available file descriptors to
read from.  Writing may block, because epoll is not used to verify
whether a socket is willing to accept data..
