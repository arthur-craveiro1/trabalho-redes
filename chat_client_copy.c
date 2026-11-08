#include "benchmark_common.h"

/* Cada execucao e um cliente. O script inicia N processos independentes. */
static socket_t connect_with_retry(const struct sockaddr_in *address) {
    double deadline = monotonic_seconds() + 120.0;
    while (monotonic_seconds() < deadline) {
        socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == BAD_SOCKET) return BAD_SOCKET;
        if (nonblocking(fd) != 0) { CLOSE_SOCKET(fd); return BAD_SOCKET; }
        int connected = connect(fd, (const struct sockaddr *)address, sizeof(*address));
        if (connected == 0) return fd;
        if (temporary_error() && wait_socket(fd, 1, monotonic_seconds() + 1.0) == 1) {
            int error = 0;
#ifdef _WIN32
            int length = sizeof(error);
#else
            socklen_t length = sizeof(error);
#endif
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &length) == 0 && !error)
                return fd;
        }
        CLOSE_SOCKET(fd);
        pause_ms(200);
    }
    return BAD_SOCKET;
}

int main(int argc, char **argv) {
    int port, expected_count = 1, repetition = 1;
    if (argc < 3 || argc > 5 || parse_integer(argv[2], 1, 65535, &port) ||
        (argc > 3 && parse_integer(argv[3], 1, 10, &expected_count)) ||
        (argc > 4 && parse_integer(argv[4], 1, 10000, &repetition))) {
        fprintf(stderr, "Uso: %s <IPv4-servidor> <porta> [clientes=1] [repeticao=1]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (network_init() != 0) return EXIT_FAILURE;
    socket_t fd = BAD_SOCKET;
    char *buffer = NULL;
    int result = EXIT_FAILURE;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
        fprintf(stderr, "IPv4 invalido. Use o IP do computador servidor.\n");
        goto cleanup;
    }
    fd = connect_with_retry(&address);
    if (fd == BAD_SOCKET) {
        fprintf(stderr, "Nao foi possivel conectar em 120 s. Confira IP, porta e firewall.\n");
        goto cleanup;
    }
    char line[128], extra;
    snprintf(line, sizeof(line), "BENCH1 %d %d\n", expected_count, repetition);
    double prepare_deadline = monotonic_seconds() + 130.0;
    if (send_control(fd, line, prepare_deadline) ||
        read_control(fd, line, sizeof(line), prepare_deadline)) goto cleanup;
    int warmup, duration, block_size;
    if (sscanf(line, "CONFIG %d %d %d %c", &warmup, &duration, &block_size, &extra) != 3 ||
        warmup < 1 || warmup > 60 || duration < 1 || duration > 3600 ||
        block_size < 1024 || block_size > 4194304) goto cleanup;
    buffer = malloc((size_t)block_size);
    if (!buffer) goto cleanup;
    memset(buffer, 'X', (size_t)block_size);
    if (send_control(fd, "READY\n", prepare_deadline) ||
        read_control(fd, line, sizeof(line), prepare_deadline) || strcmp(line, "GO")) goto cleanup;

    /* O servidor determina o fim. O prazo extra detecta servidor travado.
       Socket nao bloqueante permite receber STOP mesmo se o envio estiver cheio. */
    double watchdog = monotonic_seconds() + warmup + duration + 15.0;
    uint64_t sent = 0;
    int offset = 0, stopped = 0;
    while (monotonic_seconds() < watchdog) {
        char control;
        int incoming = (int)recv(fd, &control, 1, MSG_PEEK);
        if (incoming > 0) {
            if (read_control(fd, line, sizeof(line), watchdog) || strcmp(line, "STOP")) goto cleanup;
            stopped = 1;
            break;
        }
        if (incoming == 0 || (incoming < 0 && !temporary_error())) goto cleanup;
        int n = (int)send(fd, buffer + offset, block_size - offset, 0);
        if (n > 0) {
            sent += (uint64_t)n;
            offset += n;
            if (offset == block_size) offset = 0;
        } else if (n == 0 || !temporary_error()) goto cleanup;
        else {
            /* No maximo 20 ms sem checar o fluxo reverso. Sem pausa se ha vazao. */
            double next_check = monotonic_seconds() + 0.020;
            if (next_check > watchdog) next_check = watchdog;
            if (wait_socket(fd, 1, next_check) < 0) goto cleanup;
        }
    }
    if (!stopped) { fprintf(stderr, "Servidor nao encerrou o teste no prazo.\n"); goto cleanup; }
    if (shutdown(fd, SHUT_WRITE) != 0 ||
        read_control(fd, line, sizeof(line), monotonic_seconds() + 12.0) || strcmp(line, "OK"))
        goto cleanup;
    printf("Cliente concluido: %" PRIu64 " bytes enviados (inclui aquecimento e drenagem).\n", sent);
    printf("O throughput oficial e calculado SOMENTE pelo servidor.\n");
    result = EXIT_SUCCESS;

cleanup:
    free(buffer);
    if (fd != BAD_SOCKET) CLOSE_SOCKET(fd);
    network_cleanup();
    if (result != EXIT_SUCCESS) fprintf(stderr, "Falha no cliente; confira o log do servidor.\n");
    return result;
}
