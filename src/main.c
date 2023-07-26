// #include "socket/socket.h"
#include "buffer/buffer-reader.h"
#include "cert/cert.h"
#include "tls/tls-client.h"
#include "tls/tls-server.h"

#include <netinet/in.h>
#include <openssl/err.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <openssl/ssl.h>
#include <string.h>
#include <sys/select.h>

#define SERVER_PORT 8080
#define ROOT_CA_CERTIFICATE_LOCATION "cert/cert-test/rootCA.pem"
#define ROOT_CA_KEY_LOCATION "cert/cert-test/rootCA.key"
#define MAX_CONNECTIONS 20

/*
 * Creates factory of SSL connections, specifying that we want to create a
 * TLS server.
 *
 * @return SSL_CTX -> Configured context.
 */
SSL_CTX *create_ssl_context() {
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    // Specify method.
    method = TLS_server_method();

    ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("(error) Unable to create SSL context\n");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    return ctx;
}

/*
 * Configures the certificate used by the server when any connection asks for a
 * certificate.
 *
 * @param SSL_CTX *ctx -> Context used to create the SSL connections.
 */
void configure_ssl_context(SSL_CTX *ctx, char *hostname) {

    EVP_PKEY *key = NULL;
    X509 *crt = NULL;

    generate_certificate(ROOT_CA_KEY_LOCATION, ROOT_CA_CERTIFICATE_LOCATION,
                         &key, &crt, hostname);
    // Set certificate
    if (SSL_CTX_use_certificate(ctx, crt) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    // Set key
    if (SSL_CTX_use_PrivateKey(ctx, key) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
}

void update_FDSET_with_all_connected_sockets(
    struct ssl_connection *ssl_connections, fd_set *user_fds, int *user_max_fd,
    fd_set *host_fds, int *host_max_fd) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        int client_fd = ssl_connections[i].user.fd;
        int server_fd = ssl_connections[i].host.fd;

        // Add sd to the list of select.
        if (client_fd > 0)
            FD_SET(client_fd, user_fds);

        if (server_fd > 0)
            FD_SET(server_fd, host_fds);

        // Find the max value of sd, to the select function.
        if (client_fd > *user_max_fd)
            *user_max_fd = client_fd;

        if (server_fd > *host_max_fd)
            *host_max_fd = server_fd;
    }
}

int find_empty_position_in_ssl_connection_list(
    struct ssl_connection *ssl_connections) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {

        // If position is empty
        if (ssl_connections[i].user.fd == 0) {
            return i;
        }
    }

    return -1;
}

void clean_data_in_SSL_connection(struct ssl_connection *ssl_connection) {
    ssl_connection->user.fd = 0;
    ssl_connection->user.connection = NULL;

    ssl_connection->host.fd = 0;
    ssl_connection->host.connection = NULL;

    memset(ssl_connection->hostname, 0, DOMAIN_MAX_SIZE);
    memset(ssl_connection->port, 0, PORT_MAX_SIZE);
}

int main(int argc, char *argv[]) {

    // Ignore broken pipe signals.
    signal(SIGPIPE, SIG_IGN);

    SSL_CTX *ctx = create_ssl_context();

    // Add the correct address to the tcp socket.
    struct sockaddr_in server_address;
    set_address(&server_address, INADDR_ANY, SERVER_PORT);

    // Create socket and returng the FD.
    int server_fd = create_server_socket(server_address, 8080);

    struct ssl_connection ssl_connections[MAX_CONNECTIONS];
    fd_set user_fds, host_fds;

    // Initialize all sockets with zero.
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        clean_data_in_SSL_connection(&ssl_connections[i]);
    }

    while (1) {
        FD_ZERO(&user_fds);
        FD_ZERO(&host_fds);
        FD_SET(server_fd, &user_fds);
        int user_max_fd = server_fd;
        int host_max_fd = 0;

        update_FDSET_with_all_connected_sockets(
            ssl_connections, &user_fds, &user_max_fd, &host_fds, &host_max_fd);

        if (select(user_max_fd + 1, &user_fds, NULL, NULL, NULL) < 0) {
            printf("(error) Error in user select!\n");
            exit(0);
        }

        // New connection to the server
        if (FD_ISSET(server_fd, &user_fds)) {
            int empty_position =
                find_empty_position_in_ssl_connection_list(ssl_connections);
            printf("(info) Empty position: %d\n", empty_position);
            create_TLS_connection_with_user(
                ctx, &ssl_connections[empty_position], server_fd);
            create_TLS_connection_with_host_with_changed_SNI(
                ctx, &ssl_connections[empty_position]);

            continue;
        }

        // Read all sockets to see if message has arrived.
        int current_user_fd;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            current_user_fd = ssl_connections[i].user.fd;

            if (FD_ISSET(current_user_fd, &user_fds)) {
                bool end_connection = false;
                char *request_body = read_data_from_ssl(
                    ssl_connections[i].user.connection, &end_connection);

                write_data_in_ssl(ssl_connections[i].host.connection,
                                  request_body);
                printf("(debug) Message sent: %s\n", request_body);
            }
        }

        if (select(host_max_fd + 1, &host_fds, NULL, NULL, NULL) < 0) {
            printf("(error) Error in host select!\n");
            exit(0);
        }

        // Read all sockets to see if message has arrived.
        int current_host_fd;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            current_host_fd = ssl_connections[i].host.fd;

            if (FD_ISSET(current_host_fd, &host_fds)) {
                bool end_connection = false;
                char *response_body = read_data_from_ssl(
                    ssl_connections[i].host.connection, &end_connection);

                write_data_in_ssl(ssl_connections[i].user.connection,
                                  response_body);
                printf("(debug) Response: %s\n", response_body);

                if (end_connection) {
                    printf("Tem que terminar!\n");
                }
            }
        }

        printf("Finished!\n");
    }

    return 0;
}
