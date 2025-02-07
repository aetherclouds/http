a simple HTTP (not HTTP**S**) server just good enough that I could host my own website ([erikz.xyz](https://erikz.xyz)).
## usage
`server [<bind address> <bind port>]` (default `[::]:8008` or `0.0.0.0:8008`)
## building
### flags
they may be used like so: `make <target> FLAGS="-DIPV4 -DNOSENDFILE <your flags>"`
- `IPV4`: use IPv4 instead of IPv6
- `NOSENDFILE`: don't use `sendfile` and fall back to `write`

NOTE: IPv6 mode still supports IPv4. I don't know why or how, surely it's in a specification somewhere, but it's nice. IPv4 connections then look like (example): `::ffff:35.227.62.178:60660`

### binary
- `make prod` or `make bin/server`
## TODO:
(along with `TODO:`s within the source files)
- `errno` is not thread safe. use return values or prefer thread-safe functions
- enable choosing n. of threads at runtime
