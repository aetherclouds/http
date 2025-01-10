#pragma once
#define _POSIX_C_SOURCE 199309L

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

// `format, ##__VA_ARGS__` "consumes" comma if `__VA_ARGS__` is empty
#define logformat(format, ...) "%s:%d (%s) " format "\n",  __FILE__, __LINE__, __func__ __VA_OPT__(,) __VA_ARGS__
#define log(format, ...) printf(logformat(format, ##__VA_ARGS__))
#define log_error(format, ...) fprintf(stderr, logformat(format, ##__VA_ARGS__))
#define log_errno() fprintf(stderr, logformat("errno: %s", strerror(errno)))
#define exit_errno() {log_errno(); exit(EXIT_FAILURE);}
#define bail(cond, format, ...) if (cond) {log_error(format, ##__VA_ARGS__); exit(EXIT_FAILURE);}
#define bail_errno(cond) if (cond) exit_errno()

#define countof(x) (sizeof(x)/sizeof(x[0]))
#define cstrncmp(s1, s2) strncmp(s1, s2, sizeof(s2)-1)
#define HTON_IP(a, b, c, d) (a<<8*0|b<<8*1|c<<8*2|d<<8*3)
#define HTON_PORT(port) (uint16_t)((uint8_t)port<<8|(uint16_t)port>>8)

const int FALSE = 0;
const int TRUE = 1;

#ifndef IPV4
const int AF = AF_INET6;
const struct in6_addr ANY_ADDR = IN6ADDR_ANY_INIT;
#else
const int AF = AF_INET;
const struct in_addr ANY_ADDR = {.s_addr = HTON_IP(0, 0, 0, 0)};
#endif

int compare_ipv6(struct in6_addr *a, struct in6_addr *b) {return memcmp(a, b, sizeof(struct in6_addr));}

// https://blog.habets.se/2022/10/No-way-to-parse-integers-in-C.html
unsigned short strtous(const char *nptr, char **restrict endptr, int base)
{
    long ret = strtol(nptr, endptr, base);
    if (UINT16_MAX < ret || ret < 0) {
        errno = ERANGE;
        return 0;
    }
    return strtoul(nptr, endptr, base);
}
