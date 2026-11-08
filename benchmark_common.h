#ifndef BENCHMARK_COMMON_H
#define BENCHMARK_COMMON_H

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET socket_t;
#define BAD_SOCKET INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#define SHUT_WRITE SD_SEND
static LARGE_INTEGER clock_frequency;
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
typedef int socket_t;
#define BAD_SOCKET (-1)
#define CLOSE_SOCKET close
#define SHUT_WRITE SHUT_WR
#endif

/* Compatibilidade: GCC no Linux ou MinGW-w64 no Windows. */
static inline int network_init(void) {
#ifdef _WIN32
    WSADATA data;
    if (!QueryPerformanceFrequency(&clock_frequency)) return -1;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN);
    return 0;
#endif
}

static inline void network_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

/* Relogio monotonicamente crescente: ajustes na hora do PC nao o alteram. */
static inline double monotonic_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (double)value.QuadPart / (double)clock_frequency.QuadPart;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
#endif
}

static inline void pause_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
#endif
}

static inline int socket_error(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static inline int temporary_error(void) {
    int err = socket_error();
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == EINPROGRESS;
#endif
}

static inline int nonblocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* Retorna 1 se pronto, 0 no prazo final e -1 em erro. */
static inline int wait_socket(socket_t fd, int writing, double deadline) {
    for (;;) {
        double remaining = deadline - monotonic_seconds();
        if (remaining <= 0) return 0;
        struct timeval timeout;
        timeout.tv_sec = (long)remaining;
        timeout.tv_usec = (long)((remaining - (double)timeout.tv_sec) * 1e6);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
#ifdef _WIN32
        int nfds = 0;
#else
        int nfds = fd + 1;
#endif
        int result = select(nfds, writing ? NULL : &set,
                            writing ? &set : NULL, NULL, &timeout);
        if (result >= 0) return result;
        if (!temporary_error()) return -1;
    }
}

static inline int send_control(socket_t fd, const char *text, double deadline) {
    size_t offset = 0, length = strlen(text);
    while (offset < length) {
        if (wait_socket(fd, 1, deadline) != 1) return -1;
        int n = (int)send(fd, text + offset, (int)(length - offset), 0);
        if (n > 0) offset += (size_t)n;
        else if (n == 0 || !temporary_error()) return -1;
    }
    return 0;
}

/* Leitura por byte apenas no protocolo de controle, fora da medicao. */
static inline int read_control(socket_t fd, char *line, size_t capacity,
                               double deadline) {
    size_t used = 0;
    while (used + 1 < capacity) {
        if (wait_socket(fd, 0, deadline) != 1) return -1;
        int n = (int)recv(fd, line + used, 1, 0);
        if (n == 1) {
            if (line[used] == '\n') {
                line[used] = '\0';
                return 0;
            }
            used++;
        } else if (n == 0 || !temporary_error()) return -1;
    }
    return -1;
}

static inline int parse_integer(const char *text, int low, int high, int *out) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < low || value > high) return -1;
    *out = (int)value;
    return 0;
}
#endif
