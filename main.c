#define _POSIX_C_SOURCE 199309L
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <fcntl.h>
#include <pthread.h>

#define countof(x) (sizeof(x)/sizeof(x[0]))
#define logformat(format, ...) "%s:%d (%s) " format "\n",  __FILE__, __LINE__, __func__ __VA_OPT__(,) __VA_ARGS__
#define log(format, ...) printf(logformat(format, ##__VA_ARGS__))
#define log_error(format, ...) fprintf(stderr, logformat(format, ##__VA_ARGS__))
// NOTE:  errno is unsafe in a multithreaded environment (I'm not sure what to do about it)
#define log_errno() fprintf(stderr, logformat("errno: %s", strerror(errno)))
#define exit_errno() {log_errno(); exit(EXIT_FAILURE);}
#define HTON_IP(a, b, c, d) (a<<8*0|b<<8*1|c<<8*2|d<<8*3)
#define HTON_PORT(port) (uint16_t)((uint8_t)port<<8|(uint16_t)port>>8)
#define cstrncmp(s1, s2) strncmp(s1, s2, sizeof(s2)-1)

const in_port_t HOST_PORT = 8008;
#ifndef IPV4
const struct in6_addr HOST_ADDR = IN6ADDR_LOOPBACK_INIT;
#else
const struct in_addr HOST_ADDR = {.s_addr = HTON_IP(0, 0, 0, 0)};
#endif
const int PAGE_SIZE = 4096;
const int TRUE = 1;
const struct timeval TIMEOUT = {
    .tv_sec = 2
};

#define HTTP_NEWLINE "\r\n"
#define HTTP_END "\r\n\r\n"
#define HTTP_OK "HTTP/1.1 200 OK"
#define HTTP_BAD_REQUEST_HEADER "HTTP/1.1 400 Bad Request"HTTP_END
#define HTTP_MISSING_HEADER "HTTP/1.1 404 Not Found"HTTP_END
#define HTTP_SERVER_ERROR_HEADER "HTTP/1.1 500 Internal Server Error"HTTP_END

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
};

char* content_type_str[] = {
    "text/html",
    "text/css",
    "application/javascript",
    "image/png",
    "image/webp",
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
const struct sockaddr_in6 host_address = {
    .sin6_family = AF_INET6,
    .sin6_addr = HOST_ADDR,
    .sin6_port = HTON_PORT(HOST_PORT)
    // .sin6_flowinfo = 0, // reserved field
    // .sin6_scope_id = 0 // no idea what this is
};
char host_addr_str[INET6_ADDRSTRLEN];
#else
const struct sockaddr_in host_address = {
    .sin_family = AF_INET,
    .sin_addr = HOST_ADDR,
    .sin_port = HTON_PORT(HOST_PORT)
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
    if (file_ext_str > strrchr(o->resource, (int)'\n')) file_ext_str = NULL;
    o->content_type = "";
    if (file_ext_str) {
        for (size_t i = 0; i < countof(content_type_ext); i++) {
            // TODO: is this unsafe?
            if (0 == strcmp(file_ext_str+1, content_type_ext[i])) {
                o->content_type = content_type_str[i];
                break;
            }
        }
    }

    char* version_str = strtok_r(NULL, "\n", &position);
    if (!version_str || cstrncmp(version_str, "HTTP/1.1") != 0) return -1;
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

    ssize_t received = 0, sent = 0;
    char in_buf[PAGE_SIZE], out_buf[PAGE_SIZE];
    for (;;) { // iterate over messages (http1.1 or above)
        size_t in_filled = 0, out_filled = 0;
        for (;;) { // iterate over packets
            received = read(remotefd, &in_buf[in_filled], sizeof(in_buf) - in_filled);
            if (received == 0) {
                log("connection closed on remote end");
                goto end_connection;
            }
            if (0 > received) {
                log_errno();
                goto end_error;
            }
            in_filled += received;
            log("received %ld bytes:", received);
            if (0 == strncmp(
                &in_buf[in_filled-sizeof(HTTP_END)+1], // +1 because sizeof(STRING) includes null terminator
                HTTP_END,
                sizeof(HTTP_END)-1) // don't include null terminator in check
            // NOTE: if request had a body (i.e. POST request), it would be incorrect to assume it ends here
            ) break; // end of message
            if (in_filled == sizeof(in_buf)) { // buffer limit reached
                // TODO: return 413
                goto return_server_error;
            }
        }
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
        if (-1 == access(conn_header.resource, F_OK)) {
            log_error("resource unavailable (%s)", conn_header.resource);
            goto return_missing;
        }
        int resourcefd = open(conn_header.resource, O_RDONLY);
        if (0 > resourcefd) {log_errno(); goto return_server_error;};

        struct stat fst;
        if (0 != stat(conn_header.resource, &fst)) {log_errno(); goto return_server_error;}

        // ANSWER REQUEST

        out_filled = snprintf(out_buf, sizeof(out_buf),
            HTTP_OK HTTP_NEWLINE
            "Content-Length: %ld" HTTP_NEWLINE
            "Content-Type: %s" HTTP_NEWLINE
            HTTP_NEWLINE,

            fst.st_size,
            conn_header.content_type
        );
        if (sizeof(out_buf) == out_filled && out_buf[out_filled-1] != '\00') {
            // header response is too big
            // TODO: return 413 (?)
            goto return_server_error;
        }
        sent = write(remotefd, &out_buf, strlen(out_buf));
        // past this point, if we error, it means server/critical failure or socket is broken.
        // let's not even attempt to return an error - just close the connection.
        if (0 >= sent) {
            log_errno();
            goto end_connection;
        }

        // RESPONSE BODY

        ssize_t file_read;
        #if TYPE != 1 && TYPE != 2
        #define TYPE 2
        //#error "define TYPE 1 or 2"
        #endif
        #if TYPE == 1
        bool continue_reading = true;
        for (;;) {
            // fill buffer before sending
            out_filled = 0;
            for (;;) {
                // file descriptor seeks forward on every read
                file_read = read(
                    resourcefd,
                    &out_buf[out_filled],
                    sizeof(out_buf)-out_filled
                );
                if (0 >= file_read) {
                    if (0 == file_read) {
                        // reached EOF
                        continue_reading = false;
                        break;
                    }
                    // else:
                    log_errno();
                    goto end_connection;
                }
                out_filled += file_read;
                if (out_filled == sizeof(out_buf)) {
                    // buffer full
                    break; // write to socket and continue reading file
                }
            }
            // checkpoint: EOF or buffer full
            sent += write(remotefd, out_buf, out_filled);
            if (0 > sent) {log_errno(); goto end_connection;}
            if (!continue_reading) break;
        }
        #elif TYPE == 2
        for (;;) {
            // I've experimented with different `count` param sizes and passing
            // the size of the file seems to be the fastest.
            if (0 >= (file_read = sendfile(remotefd, resourcefd, NULL, fst.st_size))) {
                if (0 > file_read) log_errno();
                // broken pipe, client might have closed connection prematurely
                if (EPIPE == file_read) return -1;
                break;
            } else sent += file_read;

        }
        #endif
        if (0 != close(resourcefd))

        log("sent %ld+%ld bytes (header+content)", sent-fst.st_size, fst.st_size);
        if (!conn_header.keep_alive) goto end_connection;
        continue;
        return_missing:
            sent += write(remotefd, &HTTP_MISSING_HEADER, sizeof(HTTP_MISSING_HEADER)-1);
            goto return_fail;
        return_bad_request:
            sent += write(remotefd, &HTTP_BAD_REQUEST_HEADER, sizeof(HTTP_BAD_REQUEST_HEADER)-1);
            goto return_fail;
        return_server_error:
            sent += write(remotefd, &HTTP_SERVER_ERROR_HEADER, sizeof(HTTP_SERVER_ERROR_HEADER)-1);
            goto return_fail;
        return_fail:
            log("sent %ld bytes", sent);
            if (!conn_header.keep_alive) goto end_connection;
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
        * 1. casting to long since bigger->smaller cast warning is only for void* (route chosen)
        * 2. -Wno-int-to-void-pointer-cast (or wrap in some pragma that localizes it) */
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

int main(void) {
    if (NULL == getcwd(root, countof(root))) exit_errno();
    printf("root dir:\t%s\n", root);
    // signal(SIGTERM, handle_exit);
    signal(SIGINT, handle_exit);
    #ifndef IPV4
    hostfd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    #else
    hostfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    #endif
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
    inet_ntop(AF_INET6, &host_address.sin6_addr, host_addr_str, INET6_ADDRSTRLEN);
    printf("http://[%s]:%d\n", host_addr_str, HOST_PORT);
    #else
    inet_ntop(AF_INET, &host_address.sin_addr, host_addr_str, INET_ADDRSTRLEN);
    printf("http://%s:%d\n", host_addr_str, HOST_PORT);
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
