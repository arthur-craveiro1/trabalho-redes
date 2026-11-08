#include "benchmark_common.h"
#include <pthread.h>

#define MAX_CLIENTS 10
typedef struct {
    socket_t fd;
    pthread_t thread;
    char *buffer;
    uint64_t bytes;
    int failed;
} Client;

/* O mutex coordena apenas o inicio; cada thread possui seu contador. */
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_condition = PTHREAD_COND_INITIALIZER;
static int ready_threads, released, cancelled;
static double measure_start, measure_end;
static int block_size;

static void *handle_client(void *argument) {
    Client *client = argument;
    pthread_mutex_lock(&gate_mutex);
    ready_threads++;
    pthread_cond_broadcast(&gate_condition);
    while (!released) pthread_cond_wait(&gate_condition, &gate_mutex);
    int abort_test = cancelled;
    pthread_mutex_unlock(&gate_mutex);
    if (abort_test) return NULL;

    if (send_control(client->fd, "GO\n", measure_start) != 0) {
        client->failed = 1;
        return NULL;
    }
    uint64_t received_bytes = 0;
    while (monotonic_seconds() < measure_end) {
        int n = (int)recv(client->fd, client->buffer, block_size, 0);
        double received_at = monotonic_seconds();
        if (n > 0) {
            /* Bytes de aquecimento e recepcoes concluidas apos o fim nao contam. */
            if (received_at >= measure_start && received_at < measure_end)
                received_bytes += (uint64_t)n;
        } else if (n == 0) {
            client->failed = 1;
            break;
        } else if (temporary_error()) {
            int ready = wait_socket(client->fd, 0, measure_end);
            if (ready < 0) client->failed = 1;
            if (ready <= 0) break;
        } else {
            client->failed = 1;
            break;
        }
    }

    /* Publica uma vez: evita escritas concorrentes em linhas de cache vizinhas. */
    client->bytes = received_bytes;

    /* STOP via fluxo reverso. Drena dados em voo SEM contabiliza-los, para
       nao fechar uma conexao com dados pendentes e provocar reset TCP. */
    double drain_deadline = monotonic_seconds() + 10.0;
    if (send_control(client->fd, "STOP\n", drain_deadline) != 0)
        client->failed = 1;
    int eof = 0;
    while (wait_socket(client->fd, 0, drain_deadline) == 1) {
        int n = (int)recv(client->fd, client->buffer, block_size, 0);
        if (n == 0) { eof = 1; break; }
        if (n < 0 && !temporary_error()) break;
    }
    if (!eof || client->bytes == 0) client->failed = 1;
    if (send_control(client->fd, client->failed ? "ERROR\n" : "OK\n",
                     monotonic_seconds() + 2.0) != 0) client->failed = 1;
    return NULL;
}

int main(int argc, char **argv) {
    int port, count, duration, warmup = 3, repetition = 1;
    block_size = 65536;
    if (argc < 4 || argc > 8 ||
        parse_integer(argv[1], 1, 65535, &port) ||
        parse_integer(argv[2], 1, MAX_CLIENTS, &count) ||
        parse_integer(argv[3], 1, 3600, &duration) ||
        (argc > 4 && parse_integer(argv[4], 1, 60, &warmup)) ||
        (argc > 5 && parse_integer(argv[5], 1024, 4194304, &block_size)) ||
        (argc > 7 && parse_integer(argv[7], 1, 10000, &repetition))) {
        fprintf(stderr, "Uso: %s <porta> <clientes 1..10> <segundos> "
                "[aquecimento=3] [bloco=65536] [csv=resultados.csv] [repeticao=1]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    const char *csv_path = argc > 6 ? argv[6] : "resultados.csv";
    if (network_init() != 0) return EXIT_FAILURE;
    setvbuf(stdout, NULL, _IOLBF, 0);
    Client clients[MAX_CLIENTS] = {0};
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = BAD_SOCKET;
    int created = 0, result = EXIT_FAILURE;
    FILE *csv = NULL;
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == BAD_SOCKET) goto cleanup;
    int option = 1;
#ifdef _WIN32
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&option, sizeof(option)) != 0) goto cleanup;
#else
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &option, sizeof(option)) != 0) goto cleanup;
#endif
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, MAX_CLIENTS) != 0 || nonblocking(listener) != 0) {
        fprintf(stderr, "Falha ao abrir porta %d (erro %d).\n", port, socket_error());
        goto cleanup;
    }
    csv = fopen(csv_path, "a+");
    if (!csv) { perror(csv_path); goto cleanup; }
    if (fseek(csv, 0, SEEK_END) != 0) goto cleanup;
    if (ftell(csv) == 0) {
        fprintf(csv, "clientes,repeticao,bloco_bytes,aquecimento_s,duracao_s,"
                "bytes_recebidos,throughput_mbps,valido");
        for (int i = 1; i <= MAX_CLIENTS; i++) fprintf(csv, ",bytes_cliente_%d", i);
        fprintf(csv, "\n");
        if (fflush(csv) != 0) goto cleanup;
    }
    printf("Aguardando %d clientes na porta %d; repeticao %d.\n", count, port, repetition);

    double connection_deadline = monotonic_seconds() + 120.0;
    for (int i = 0; i < count; i++) {
        if (wait_socket(listener, 0, connection_deadline) != 1) {
            fprintf(stderr, "Tempo esgotado aguardando os clientes.\n");
            goto cleanup;
        }
        clients[i].fd = accept(listener, NULL, NULL);
        if (clients[i].fd == BAD_SOCKET || nonblocking(clients[i].fd) != 0) goto cleanup;
        char line[128], extra;
        int expected_count, expected_rep;
        if (read_control(clients[i].fd, line, sizeof(line), connection_deadline) ||
            sscanf(line, "BENCH1 %d %d %c", &expected_count, &expected_rep, &extra) != 2 ||
            expected_count != count || expected_rep != repetition) {
            fprintf(stderr, "Cliente usa cenario/repeticao ou protocolo diferente.\n");
            goto cleanup;
        }
        snprintf(line, sizeof(line), "CONFIG %d %d %d\n", warmup, duration, block_size);
        if (send_control(clients[i].fd, line, connection_deadline) ||
            read_control(clients[i].fd, line, sizeof(line), connection_deadline) ||
            strcmp(line, "READY")) goto cleanup;
        clients[i].buffer = malloc((size_t)block_size);
        if (!clients[i].buffer) goto cleanup;
        if (pthread_create(&clients[i].thread, NULL, handle_client, &clients[i]) != 0)
            goto cleanup;
        created++;
        printf("Cliente %d/%d pronto.\n", i + 1, count);
    }
    CLOSE_SOCKET(listener);
    listener = BAD_SOCKET;
    pthread_mutex_lock(&gate_mutex);
    while (ready_threads != count) pthread_cond_wait(&gate_condition, &gate_mutex);
    measure_start = monotonic_seconds() + (double)warmup;
    measure_end = measure_start + (double)duration;
    released = 1;
    pthread_cond_broadcast(&gate_condition);
    pthread_mutex_unlock(&gate_mutex);
    printf("Transmitindo: %d s de aquecimento + %d s de medicao.\n", warmup, duration);

    for (int i = 0; i < created; i++) pthread_join(clients[i].thread, NULL);
    created = 0;
    uint64_t total = 0;
    int valid = 1;
    for (int i = 0; i < count; i++) {
        total += clients[i].bytes;
        if (clients[i].failed) valid = 0;
        printf("Cliente %d: %" PRIu64 " bytes medidos%s.\n", i + 1,
               clients[i].bytes, clients[i].failed ? " (FALHA)" : "");
    }
    double elapsed = measure_end - measure_start;
    double mbps = (double)total * 8.0 / (elapsed * 1e6);
    fprintf(csv, "%d,%d,%d,%d,%.9f,%" PRIu64 ",%.6f,%d",
            count, repetition, block_size, warmup, elapsed, total, mbps, valid);
    for (int i = 0; i < MAX_CLIENTS; i++) fprintf(csv, ",%" PRIu64, clients[i].bytes);
    fprintf(csv, "\n");
    if (fflush(csv) != 0) { perror("Gravacao CSV"); goto cleanup; }
    printf("%s: %.3f Mbps agregados. CSV: %s\n", valid ? "VALIDO" : "INVALIDO", mbps, csv_path);
    result = valid ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    if (created) {
        pthread_mutex_lock(&gate_mutex);
        cancelled = released = 1;
        pthread_cond_broadcast(&gate_condition);
        pthread_mutex_unlock(&gate_mutex);
        for (int i = 0; i < created; i++) pthread_join(clients[i].thread, NULL);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != BAD_SOCKET) CLOSE_SOCKET(clients[i].fd);
        free(clients[i].buffer);
    }
    if (listener != BAD_SOCKET) CLOSE_SOCKET(listener);
    if (csv && fclose(csv) != 0) result = EXIT_FAILURE;
    pthread_cond_destroy(&gate_condition);
    pthread_mutex_destroy(&gate_mutex);
    network_cleanup();
    if (result != EXIT_SUCCESS) fprintf(stderr, "Ensaio nao concluido com sucesso.\n");
    return result;
}
