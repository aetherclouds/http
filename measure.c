#include "common.h"
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

const char send_header_fmt[] =
"GET /%s HTTP/1.1\r\n"
"Host: [::1]:8008\r\n"
"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0\r\n"
"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/png,image/svg+xml,*/*;q=0.8\r\n"
"Accept-Language: en-US,en;q=0.7,zh;q=0.3\r\n"
"Accept-Encoding: gzip, deflate, br, zstd\r\n"
"Connection: close\r\n"
"Upgrade-Insecure-Requests: 1\r\n"
"Sec-Fetch-Dest: document\r\n"
"Sec-Fetch-Mode: navigate\r\n"
"Sec-Fetch-Site: none\r\n"
"Sec-Fetch-User: ?1\r\n"
"Priority: u=0, i\r\n"
"\r\n";
char send_header[sizeof(send_header_fmt) + 256];

struct sockaddr_in hostaddr = {
    .sin_family = AF_INET,
    .sin_addr = ANY_ADDR4, // a bit leaky since incoming data will be broadcasted to every local device
    .sin_port = HTON_PORT(9999),
};
struct sockaddr_in remoteaddr = {
    .sin_family = AF_INET
};

#define USAGE_STR "usage: measure <samples> <ipv4> <port> <resource(optional)>"

int main(int argc, char *argv[]) {
    int result;
    bail(3 > argc || argc > 5, USAGE_STR);

    int n_samples = strtol(argv[1], NULL, 10);
    double samples[n_samples];

    result = inet_pton(AF_INET, argv[2], &remoteaddr.sin_addr);
    bail(0 == result, USAGE_STR);
    bail_errno(1 != result);

    remoteaddr.sin_port = htons(strtol(argv[3], NULL, 10));

    char* resource;
    if (argc == 5) resource = argv[4];
    else resource = (char*)&FALSE;
    snprintf(send_header, sizeof(send_header), send_header_fmt, resource);

    double filesize = -1;
    for (int i = 0; i < n_samples; i++) {
        long errored = 0;
        int sockfd;
        int received, sent;
        char received_buf[PAGE_SIZE*16];
        struct timespec start, end;

        bail_errno(0 > (sockfd = socket(AF_INET, SOCK_STREAM, 0)));
        bail_errno(0 != setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &TRUE, sizeof(TRUE))) // enable faster program restarts
        bail_errno(0 != setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &TRUE, sizeof(TRUE))) // disable Nagle's algorithm, probably unnecessary
        bail_errno(0 != bind(sockfd, (struct sockaddr*)&hostaddr, sizeof(hostaddr)));

        for (;;) {
            if (0 > connect(sockfd, (struct sockaddr*)&remoteaddr, sizeof(remoteaddr))) {
                ++errored;
                // TODO: doesn't show up on my terminal without a newline. buffered?
                printf(logformat_nonl("sample %d, attempt %ld: address taken, retrying\r", i, errored));
            } else break;
        }


        bail_errno(0 > (sent = write(sockfd, send_header, strlen(send_header))));

        clock_gettime(CLOCK_MONOTONIC_RAW, &start);
        if (0 >= (received = read(sockfd, received_buf, sizeof(received_buf)))) {
            if (0 > received) log_errno();
        } else {
            char *content_length_str;
            // if (NULL != (content_length_str = strstr(received_buf, "Content-Length"))) {
            bail(received_buf == strstr(received_buf, "HTTP/1.1 404 Not Found"), "\"%s\" resource path 404'd", resource);
            bail(NULL == (content_length_str = strstr(received_buf, "Content-Length")), "no Content-Length header found");
            filesize = strtol(content_length_str+sizeof("Content-Length:")-1 /* for \0 */, NULL, 10);
        }
        for (;;) {
            if (0 >= (received = read(sockfd, received_buf, sizeof(received_buf)))) {
                if (0 > received) log_errno();
                break;
            }
        }
        clock_gettime(CLOCK_MONOTONIC_RAW, &end);

        close(sockfd);

        double time =
              end.tv_sec - start.tv_sec
            + (end.tv_nsec - start.tv_nsec) * 1e-9;
        samples[i] = time;
        // MB not MiB!!!!
        printf("took %fs (%fmb/s)", time, (filesize/1000/1000)/time /*, end.tv_sec - start.tv_sec, end.tv_nsec - start.tv_nsec*/);
        if (errored > 0) printf(" (%ld errors)", errored);
        puts("                           ");
    }

    double average;
    for (int i = 0; i < n_samples; i++) {
        average += samples[i] / (float)n_samples;
    }

    printf("avg. %fs (%fmb/s)\nsamples: %d\n", average, (filesize/1000/1000)/average, n_samples);

    return 0;
}
