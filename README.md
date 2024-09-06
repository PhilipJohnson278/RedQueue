What is RedQueue?
--------------

RedQueue is a MQ (Message Queue) server bases on Redis stream data structures. What this means is that RedQueue as a Message Queue server, provides access to MQ data via a set of Redis stream commands, which are sent using Redis' *server-client* model with TCP sockets and a simple protocol. So different processes can query and modify the same MQ data in a shared way.

Building RedQueue
--------------

RedQueue can be compiled and used on Linux, OSX, OpenBSD, NetBSD, FreeBSD.
We support big endian and little endian architectures, and both 32 bit
and 64 bit systems.

It may compile on Solaris derived systems (for instance SmartOS) but our
support for this platform is *best effort* and RedQueue is not guaranteed to
work as well as in Linux, OSX, and \*BSD.

It is as simple as:

    % make

To build with TLS support, you'll need OpenSSL development libraries (e.g.
libssl-dev on Debian/Ubuntu) and run:

    % make BUILD_TLS=yes

To build with systemd support, you'll need systemd development libraries (such 
as libsystemd-dev on Debian/Ubuntu or systemd-devel on CentOS) and run:

    % make USE_SYSTEMD=yes

To append a suffix to RedQueue program names, use:

    % make PROG_SUFFIX="-alt"

You can build a 32 bit RedQueue binary using:

    % make 32bit

After building RedQueue, it is a good idea to test it using:

    % make test

If TLS is built, running the tests with TLS enabled (you will need `tcl-tls`
installed):

    % ./utils/gen-test-certs.sh
    % ./runtest --tls


Fixing build problems with dependencies or cached build options
---------

RedQueue has some dependencies which are included in the `deps` directory.
`make` does not automatically rebuild dependencies even if something in
the source code of dependencies changes.

When you update the source code with `git pull` or when code inside the
dependencies tree is modified in any other way, make sure to use the following
command in order to really clean everything and rebuild from scratch:

    % make distclean

This will clean: jemalloc, lua, hiredis, linenoise and other dependencies.

Also if you force certain build options like 32bit target, no C compiler
optimizations (for debugging purposes), and other similar build time options,
those options are cached indefinitely until you issue a `make distclean`
command.

Fixing problems building 32 bit binaries
---------

If after building RedQueue with a 32 bit target you need to rebuild it
with a 64 bit target, or the other way around, you need to perform a
`make distclean` in the root directory of the RedQueue distribution.

In case of build errors when trying to build a 32 bit binary of RedQueue, try
the following steps:

* Install the package libc6-dev-i386 (also try g++-multilib).
* Try using the following command line instead of `make 32bit`:
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

Allocator
---------

Selecting a non-default memory allocator when building RedQueue is done by setting
the `MALLOC` environment variable. RedQueue is compiled and linked against libc
malloc by default, with the exception of jemalloc being the default on Linux
systems. This default was picked because jemalloc has proven to have fewer
fragmentation problems than libc malloc.

To force compiling against libc malloc, use:

    % make MALLOC=libc

To compile against jemalloc on Mac OS X systems, use:

    % make MALLOC=jemalloc

Monotonic clock
---------------

By default, RedQueue will build using the POSIX clock_gettime function as the
monotonic clock source.  On most modern systems, the internal processor clock
can be used to improve performance.  Cautions can be found here: 
    http://oliveryang.net/2015/09/pitfalls-of-TSC-usage/

To build with support for the processor's internal instruction clock, use:

    % make CFLAGS="-DUSE_PROCESSOR_CLOCK"

Verbose build
-------------

RedQueue will build with a user-friendly colorized output by default.
If you want to see a more verbose output, use the following:

    % make V=1

Running RedQueue
-------------

To run RedQueue with the default configuration, just type:

    % cd src
    % ./redqueue-server

If you want to provide your redqueue.conf, you have to run it using an additional
parameter (the path of the configuration file):

    % cd src
    % ./redqueue-server /path/to/redqueue.conf

It is possible to alter the RedQueue configuration by passing parameters directly
as options using the command line. Examples:

    % ./redqueue-server --port 9999 --replicaof 127.0.0.1 6379
    % ./redqueue-server /etc/redqueue/6379.conf --loglevel debug

All the options in redqueue.conf are also supported as options using the command
line, with exactly the same name.

Running RedQueue with TLS:
------------------

Please consult the [TLS.md](TLS.md) file for more information on
how to use RedQueue with TLS.

Installing RedQueue
-----------------

In order to install RedQueue binaries into /usr/local/bin, just use:

    % make install

You can use `make PREFIX=/some/other/directory install` if you wish to use a
different destination.

`make install` will just install binaries in your system, but will not configure
init scripts and configuration files in the appropriate place. This is not
needed if you just want to play a bit with RedQueue, but if you are installing
it the proper way for a production system, we have a script that does this
for Ubuntu and Debian systems:

    % cd utils
    % ./install_server.sh

_Note_: `install_server.sh` will not work on Mac OSX; it is built for Linux only.

The script will ask you a few questions and will setup everything you need
to run RedQueue properly as a background daemon that will start again on
system reboots.

You'll be able to stop and start RedQueue using the script named
`/etc/init.d/redqueue_<portnumber>`, for instance `/etc/init.d/redqueue_6379`.

Enjoy!
