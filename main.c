#include "common.h"

#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#include <signal.h>
#include <sys/time.h>
#include <fcntl.h>
#include <pthread.h>


#define USAGE_STR "usage: server <bind address> <bind port>"

const in_port_t DEFAULT_PORT = 8008;

const struct timeval TIMEOUT = {
    .tv_sec = 2
};

#define HTTP_NEWLINE "\r\n"
#define HTTP_END "\r\n\r\n"
#define HTTP_OK "HTTP/1.1 200 OK"
// "Content-Length" is very important, without it the browser will just hang on the request until timeout.
#define NO_CONTENT_REPLY(msg, msglen) "HTTP/1.1 "msg HTTP_NEWLINE"Content-Length: " #msglen HTTP_END msg
const char HTTP_BAD_REQUEST[] = NO_CONTENT_REPLY("400 Bad Request", 15);
const char HTTP_MISSING[] = NO_CONTENT_REPLY("404 Not Found", 13);
const char HTTP_SERVER_ERROR[] = NO_CONTENT_REPLY("500 Internal Server Error", 25);
const char HTTP_REQUEST_ENTITY_TOO_LARGE[] = NO_CONTENT_REPLY("413 Request Entity Too Large", 28);

typedef enum {
    GET,
    POST,
} HttpMethod;

char* content_type_ext[] = {
    "html",
    "css",
    "js",
    "png",
    "webp",
    "gif",
};

char* content_type_str[] = {
    "text/html",
    "text/css",
    "application/javascript",
    "image/png",
    "image/webp",
    "image/gif",
};

typedef enum {
    HTTP11,
    HTTP20,
} HttpVersion;

typedef struct {
    HttpMethod method;
    HttpVersion version;
    char* content_type;
    char resource[256];
    bool keep_alive;
} HttpHeader;

uint8_t n_threads = 4; // TODO: choose at runtime
pthread_t *thread_pool;
int hostfd;
int *remotefds;

char root[256];
#ifndef IPV4
struct sockaddr_in6 host_address = {
    .sin6_family = AF_INET6,
    // .sin6_flowinfo = 0, // reserved field
    // .sin6_scope_id = 0 // no idea what this is
};
char host_addr_str[INET6_ADDRSTRLEN];
#else
struct sockaddr_in host_address = {
    .sin_family = AF_INET,
};
char host_addr_str[INET_ADDRSTRLEN];
#endif

int parse_header(char* header, HttpHeader* o) {
    char* position;
    char* str_method = strtok_r(header, " ", &position);
    if (!str_method) return -1;
    if (cstrncmp(str_method, "GET") == 0)
        o->method = GET;
    else
        return -1;

    char* str_resource = strtok_r(NULL, " ", &position);
    if (!str_resource) return -1;
    strcpy(o->resource, root);
    strcat(o->resource, str_resource);
    if (str_resource[strlen(str_resource)-1] == '/') {
        strcat(o->resource, "index.html");
    }
    log("requesting resource: %s", o->resource);

    char *file_ext_str = strrchr(o->resource, (int)'.');
    if (file_ext_str) {
        for (size_t i = 0; i < countof(content_type_ext); i++) {
            // TODO: is this unsafe?
            if (0 == strcmp(file_ext_str+1, content_type_ext[i])) {
                o->content_type = content_type_str[i];
                break;
            }
        }
    }

    char* version_str = strtok_r(NULL, "\r", &position);
    if (!version_str) return -1;
    // iterate over attributes in `Key: Value\r\n` format
    for (;;) {
        position = strchr(position, '\n')+1;
        if (NULL == position  || //     vv end of http request header
            *position == '\r' || // \r\n\r\n
            *position == '\0'   // may happen if there is an empty newline
        ) break;
        if (0 == cstrncmp(position, "Connection")) {
            strtok_r(position, ":", &position);
            char* value = position+1; // + 1 to skip over ' '
            if (cstrncmp(value, "keep-alive") == 0) o->keep_alive = true;
        }
    }
    return 0;
}

void handle_sigpipe(int signumber) {
    // if we hit sigpipe, just continue
    // read() and write() will return EPIPE, so it's still handleable
    (void)signumber;
}


int handle_connection(int remotefd) {
    int result = 0;
    #ifndef IPV4
    struct sockaddr_in6 conn_address;
    #else
    struct sockaddr_in conn_address;
    #endif
    socklen_t conn_addrlen = sizeof(conn_address);
    HttpHeader conn_header = {0};

    if (0 > (remotefd = accept(hostfd, (struct sockaddr*)&conn_address, &conn_addrlen))) {
        log_errno();
        return -1;
    }

    char ext_addr_str[INET6_ADDRSTRLEN];
    #ifndef IPV4
    inet_ntop(AF_INET6, &(conn_address.sin6_addr), ext_addr_str, INET6_ADDRSTRLEN);
    log("new connection: %s:%d\n", ext_addr_str, ntohs(conn_address.sin6_port));
    #else
    inet_ntop(AF_INET, &(conn_address.sin_addr), ext_addr_str, INET_ADDRSTRLEN);
    log("new connection: %s:%d\n", ext_addr_str, ntohs(conn_address.sin_port));
    #endif

    char in_buf[PAGE_SIZE], out_buf[PAGE_SIZE];
    for (;;) { // iterate over messages (http1.1 or above)
        ssize_t sent = 0; // per HTTP message
        /* I chose to limit the receiving HTTP header size to a reasonable limit (8KB) which is the size of in_buf.
         so we won't be reusing the buffer (i.e. resetting `in_filled` to 0). */
        #define received in_filled
        size_t in_filled = 0, out_filled = 0;
        
        // RECEIVE REQUEST

        for (;;) { // iterate over packets
            if (0 >= (result = read(remotefd, &in_buf[in_filled], sizeof(in_buf) - in_filled))) {
                if (0 > result) {
                    log_errno();
                    goto end_error;
                }
                log("connection closed on remote end");
                goto end_connection;
            }
            in_filled += result;
            if (0 == strncmp(
                &in_buf[in_filled-sizeof(HTTP_END)+1], 
                HTTP_END,
                sizeof(HTTP_END)-1) // null terminator offset
            // NOTE: if request had a body (i.e. POST request), it would be incorrect to assume it ends here
            ) break; // end of message
            // NOTE: might arrive here if request DOES have HTTP_END but is encrypted (HTTPS)
            if (in_filled >= sizeof(in_buf)) { // buffer limit reached
                goto return_entity_too_large;
            }
        }
        log("received %ld bytes:", received);

        // int dumpfile = open("./dump", O_CREAT|O_RDWR, S_IRWXU);
        // log("fd %d", dumpfile);
        // log("wrote %d", write(dumpfile, in_buf, in_filled));
        // log_errno();
        // close(dumpfile);
        // exit(EXIT_SUCCESS);
        puts("\e[0;35m");
        write(STDOUT_FILENO, in_buf, in_filled);
        puts("\e[A\e[0;39m");

        // PROCESS REQUEST

        if (-1 == parse_header(in_buf, &conn_header)) goto return_bad_request;
        struct stat fst;
        if (0 != stat(conn_header.resource, &fst)) {
            log_errno();
            if (ENOENT == errno) goto return_missing;
            goto return_server_error;
        }
        // check if file is actually a directory
        if (S_ISDIR(fst.st_mode)) goto return_missing; 
        
        int resourcefd = open(conn_header.resource, O_RDONLY);
        if (0 > resourcefd) {log_errno(); goto return_server_error;}

        // ANSWER REQUEST

        #define write_header(format, ...) out_filled += snprintf(\
            out_buf+out_filled, sizeof(out_buf)-out_filled,\
            format, ##__VA_ARGS__)

        write_header(
            HTTP_OK HTTP_NEWLINE
            "Content-Length: %ld" HTTP_NEWLINE,
            fst.st_size
        );
        if (conn_header.content_type)
            write_header(
                "Content-Type: %s" HTTP_NEWLINE,
                conn_header.content_type
            );
        write_header(HTTP_NEWLINE);

        if (sizeof(out_buf) <= out_filled
            && '\00' != out_buf[sizeof(out_buf)-1] // snprintf includes terminating null byte
            // response header is too big
        ) goto return_entity_too_large;

        // past this point, if we error, it means server/critical failure or socket is broken.
        // let's not even attempt to return an error - just close the connection.
        if (0 >= (result = write(remotefd, &out_buf, strlen(out_buf)))) {
            log_errno();
            goto end_error;
        }
        sent += result;

        // RESPONSE BODY

        #ifdef NOSENDFILE
        bool continue_reading = true;
        do { // reuse buffer if it gets full
            out_filled = 0;
            for (;;) {// `read` doesn't always return its maximum read size
                // file descriptor seeks forward on every read
                result = read(
                    resourcefd,
                    &out_buf[out_filled],
                    sizeof(out_buf)-out_filled
                );
                if (0 >= result) {
                    if (0 == result) {
                        // reached EOF
                        continue_reading = false;
                        break;
                    } // else:
                    log_errno();
                    goto end_error;
                }
                out_filled += result;
                if (out_filled == sizeof(out_buf))// buffer full
                    break; // write to socket and continue reading file
            }
            // checkpoint: EOF or buffer full
            result = write(remotefd, out_buf, out_filled);
            if (0 > result) {log_errno(); goto end_error;}
            sent += result;  
        } while (continue_reading);
        #else
        for (;;) {
            // I've experimented with different `count` param sizes and passing
            // the size of the file seems to be the fastest.
            if (0 >= (result = sendfile(remotefd, resourcefd, NULL, fst.st_size))) {
                if (0 > result) {
                    // EPIPE: broken pipe, client might have closed connection prematurely
                    log_errno();
                    return -1;
                }
                // EOF
                break;
            } 
            sent += result;
        }
        #endif
        if (0 != close(resourcefd)) log_errno();

        log("sent %ld+%ld bytes (header+content)", sent-fst.st_size, fst.st_size);
        if (!conn_header.keep_alive) goto end_connection;
        continue;
        // TODO: how to avoid repetition here?
        return_missing:
            sent += write(remotefd, HTTP_MISSING, sizeof(HTTP_MISSING)-1);
            goto return_fail;
        return_bad_request:
            sent += write(remotefd, HTTP_BAD_REQUEST, sizeof(HTTP_BAD_REQUEST)-1);
            goto return_fail;
        return_server_error:
            sent += write(remotefd, HTTP_SERVER_ERROR, sizeof(HTTP_SERVER_ERROR)-1);
            goto return_fail;
        return_entity_too_large:
            sent += write(remotefd, HTTP_REQUEST_ENTITY_TOO_LARGE, sizeof(HTTP_REQUEST_ENTITY_TOO_LARGE)-1);
            goto return_fail;
        return_fail:
            log("sent %ld bytes with error", sent);
            if (!conn_header.keep_alive) goto end_error;
            continue;
    }
    end_error:
        result = -1;
    end_connection:
        log("ending connection");
        if (0 != close(remotefd)) {
            log_errno();
            return -1;
        }
        return result;
}

void *thread_routine(void *args) {
    static struct sigaction sigpipe_action = {.sa_handler = handle_sigpipe};
    sigaction(SIGPIPE, &sigpipe_action, NULL);

    int result;
    do {
        /* we can avoid compiler warnings here by
        * 1. casting to long since bigger->smaller cast warning is only for void* (path chosen)
        * 2. -Wno-int-to-void-pointer-cast (or wrap in some pragma that suppresses it) */
        result = handle_connection((int)(long)args);
    // } while (0 == result);
    } while (1);
    return (void*)(long)result;
}


void handle_exit(int signumber) {
    (void)signumber;
    puts("\nexiting..\n");
    close(hostfd);
    for (int i = 0; i < n_threads; i++) {
        close(remotefds[i]);
    }
    // TODO: terminate threads
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
    if (1 == argc) {
        #ifndef IPV4
        host_address.sin6_addr = ANY_ADDR;
        host_address.sin6_port = HTON_PORT(DEFAULT_PORT);
        #else
        host_address.sin_addr = ANY_ADDR;
        host_address.sin_port = HTON_PORT(DEFAULT_PORT);
        #endif
    } else if (3 == argc) {
        #ifndef IPV4
        if (1 != inet_pton(AF, argv[1], &host_address.sin6_addr)) {
        #else
        if (1 != inet_pton(AF, argv[1], &host_address.sin_addr)) {
        #endif
            log("invalid address: %s", argv[1]);
            exit(EXIT_FAILURE);
        }
        #ifndef IPV4
        host_address.sin6_port = HTON_PORT(strtous(argv[2], NULL, 10));
        #else
        host_address.sin_port = HTON_PORT(strtous(argv[2], NULL, 10));
        #endif
    } else {
        log("usage: server [<bind address> <bind port>]");
        exit(EXIT_FAILURE);
    }

    bail_errno(NULL == getcwd(root, countof(root)));
    printf("root dir:\t%s\n", root);
    // signal(SIGTERM, handle_exit);
    signal(SIGINT, handle_exit);
    hostfd = socket(AF, SOCK_STREAM, IPPROTO_TCP);
    if (0 > hostfd) exit_errno();
    // drop late packets after closing socket, enabling us to quickly restart the program
    if (0 != setsockopt(hostfd, SOL_SOCKET, SO_REUSEADDR, &TRUE, sizeof(TRUE)))
        log_errno();

    if (0 != bind(hostfd, (struct sockaddr*)&host_address, sizeof(host_address)))
        exit_errno();

    if (0 != listen(hostfd, n_threads))
        exit_errno();
    // checkpoint: socket is ready for connections
    printf("socket fd:\t%d\npid:\t\t%d\nparent pid:\t%d\n", hostfd, getpid(), getppid());
    #ifndef IPV4
    inet_ntop(AF, &host_address.sin6_addr, host_addr_str, INET6_ADDRSTRLEN);
    printf("http://[%s]:%d\n", host_addr_str, HTON_PORT(host_address.sin6_port));
    #else
    inet_ntop(AF, &host_address.sin_addr, host_addr_str, HTON_PORT(host_address.sin_port));
    printf("http://%s:%d\n", host_addr_str, HTON_PORT(host_address.sin_port));
    #endif

    // setup threads
    thread_pool = malloc(n_threads * (sizeof *thread_pool + sizeof *remotefds));
    if (NULL == thread_pool) exit_errno();
    remotefds = (int*)(thread_pool + (n_threads * sizeof(*thread_pool)));
    for (int i = 0; i < n_threads; i++) {
        pthread_create(
            &thread_pool[i],
            NULL,
            thread_routine,
            /* pthread_create takes a pointer to the argument struct. however,
            * since we only have one argument, why not pass it directly? */
            (void*)&remotefds[i]
        );
    }
    for (int i = 0; i < n_threads; i++) {
        pthread_join(thread_pool[i], NULL);
    }
}
