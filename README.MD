# uring File Server

A dead simple file server built with liburing.

## Message Format

Server expects each connected client to send its desired file path to download as the first message. After that, server
simply sends all the bytes of the file in the connected socket. If a file could not be opened or was not found, a "NOT
FOUND" will be sent into the socket.

The usage can be simplified like this:

```bash
nc 127.0.0.1 12345 <<<"path/to/file.bin" >file.bin
```

## Building

You can use CMake to build this software. It's important to note that you should have `liburing` installed. I
used `libruing` version 2.5 to compile this software.

```bash
mkdir Build
cd Build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```