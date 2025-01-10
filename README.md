a simple HTTP (not HTTP**S**) server just good enough that I could host my own website ([erikz.xyz](https://erikz.xyz)).
## usage
`server [<bind address> <bind port>]` (default `0.0.0.0:8008`)
## building
### flags
- `IPV4`: use IPv4 instead of IPv6
- `NOSENDFILE`: don't use `sendfile` and fall back to `write`
they may be used like so:
- `make <target> CFLAGS="-DIPV4 -DNOSENDFILE <your flags>"`
### building
- `make prod` or `make bin/server`
### building for IPv4
`make CFLAGS=-DIPV4`
## TODO:
(along with `TODO:`s within the source files)
- `errno` is not thread safe. use return values or prefer thread-safe functions
- enable choosing n. of threads at runtime
