#include <stdio.h>
#include <stdlib.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

struct io_uring ring;

#define QUEUE_DEPTH            128
#define EVENT_TYPE_ACCEPT      0
#define EVENT_TYPE_READ_PATH   1
#define EVENT_TYPE_READ_FILE   2
#define EVENT_TYPE_WRITE       3
#define EVENT_TYPE_404         4
#define REQUEST_BUFFER_SIZE (32 * 1024)
#define NOT_FOUND_MESSAGE "NOT FOUND"
#define NOT_FOUND_MESSAGE_SIZE sizeof(NOT_FOUND_MESSAGE)

struct request {
    int client_socket_fd;
    int file_fd;
    uint8_t event_type;
    char buffer[REQUEST_BUFFER_SIZE];
};

/**
 * Setup the io_uring's ring. Must be called before anything.
 */
void setup_ring() {
    int result = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    if (result < 0) {
        perror("cannot setup queue");
        exit(1);
    }
}

/**
 * Add an accept event to the ring. Used when we want to accept connections of socket.
 * @param server_socket File descriptor of the server socket.
 * @param client_addr A pointer to a sockaddr_in struct which the data will go in.
 * @param client_addr_len The length of the client_addr struct.
 */
void add_accept_request(int server_socket, struct sockaddr_in *client_addr, socklen_t *client_addr_len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, server_socket, (struct sockaddr *) client_addr,
                         client_addr_len, 0);
    struct request *req = malloc(sizeof(*req));
    req->event_type = EVENT_TYPE_ACCEPT;
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

/**
 * After a client is connected, we expect them to send the desired file path as its first line of request.
 * @param client_socket The file descriptor of the client's socket.
 * @param req The request data in the ring request.
 */
void add_read_path_request(int client_socket, struct request *req) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    assert(sqe);
    io_uring_prep_read(sqe, client_socket, &req->buffer, REQUEST_BUFFER_SIZE, 0);
    req->client_socket_fd = client_socket;
    req->event_type = EVENT_TYPE_READ_PATH; // change the type to read path
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

/**
 * This function gets called after after we have read the first line of the socket.
 * Which contains the file path that we want to read.
 * @param req The request data in the ring request.
 */
void handle_file_path(struct request *req) {
    // Get a position in queue
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    assert(sqe);
    // Try to get the filepath
    size_t file_len = strlen(req->buffer);
    req->buffer[REQUEST_BUFFER_SIZE - 1] = '\0'; // terminate the string just in case
    if (file_len > 0 && req->buffer[file_len - 1] == '\n')
        req->buffer[file_len - 1] = '\0';
    req->file_fd = open(req->buffer, O_RDONLY);
    if (req->file_fd < 0) {
        // Fuckup
        req->event_type = EVENT_TYPE_404;
        io_uring_prep_write(sqe, req->client_socket_fd, NOT_FOUND_MESSAGE, NOT_FOUND_MESSAGE_SIZE, 0);
        io_uring_sqe_set_data(sqe, req);
        io_uring_submit(&ring);
        return;
    }
    // Otherwise, dispatch a read request from file
    req->event_type = EVENT_TYPE_READ_FILE;
    io_uring_prep_read(sqe, req->file_fd, &req->buffer, REQUEST_BUFFER_SIZE, -1);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

/**
 * This function gets called after after we have read some data from the target file.
 * @param bytes_read How many bytes we have read from the file?
 * @param req The request data in the ring request.
 */
void handle_file_read(int bytes_read, struct request *req) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    assert(sqe);
    req->event_type = EVENT_TYPE_WRITE;
    io_uring_prep_write(sqe, req->client_socket_fd, &req->buffer, bytes_read, 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

/**
 * This function gets called after after we have wrote some data to the target socket.
 * @param req The request data in the ring request.
 */
void handle_socket_write(struct request *req) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    assert(sqe);
    req->event_type = EVENT_TYPE_READ_FILE;
    io_uring_prep_read(sqe, req->file_fd, &req->buffer, REQUEST_BUFFER_SIZE, -1);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

int setup_listening_socket(int port) {
    int sock;
    struct sockaddr_in srv_addr = {};

    // Create a socket and return its FD
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("cannot create socket");
        exit(1);
    }

    // Setup the data needed for socket and bind the socket
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (const struct sockaddr *) &srv_addr, sizeof(srv_addr)) < 0) {
        perror("cannot bind socket");
        exit(1);
    }

    // Listen for connections
    if (listen(sock, 16) < 0) {
        perror("cannot listen");
        exit(1);
    }
    return sock;
}

void server_loop(int socket_fd) {
    struct io_uring_cqe *cqe;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Initialize the ring with a single accept request to accept new TCP connections
    add_accept_request(socket_fd, &client_addr, &client_addr_len);

    while (1) {
        // Wait until an event is ready in the queue...
        int result = io_uring_wait_cqe(&ring, &cqe);
        if (result < 0) {
            perror("cannot wait");
            exit(1);
        }
        struct request *req = (struct request *) cqe->user_data;
        if (cqe->res < 0) { // did the request fail?
            fprintf(stderr, "Async request failed: %s for event: %d\n",
                    strerror(-cqe->res), req->event_type);
            exit(1);
        }
        // What should we do?
        switch (req->event_type) {
            case EVENT_TYPE_ACCEPT:
                // New socket!
                add_read_path_request(cqe->res, req); // read from socket
                add_accept_request(socket_fd, &client_addr, &client_addr_len);
                break;
            case EVENT_TYPE_READ_PATH:
                // We have read the path of the file...
                handle_file_path(req);
                break;
            case EVENT_TYPE_404:
                // We just send a 404 error to user. Close the socket and free the resources
                shutdown(req->client_socket_fd, SHUT_RDWR);
                close(req->client_socket_fd);
                free(req);
                break;
            case EVENT_TYPE_READ_FILE:
                // At first check if we have read anything at all.
                if (cqe->res == 0) {
                    // EOF. Close everything
                    close(req->file_fd);
                    shutdown(req->client_socket_fd, SHUT_RDWR);
                    close(req->client_socket_fd);
                    free(req);
                    break;
                }
                // We have read data from file. Write it back to socket.
                handle_file_read(cqe->res, req);
                break;
            case EVENT_TYPE_WRITE:
                // We have written our data in the socket. Read more data...
                handle_socket_write(req);
                break;
        }
        // Done
        io_uring_cqe_seen(&ring, cqe);
    }
}

int main() {
    setup_ring();
    int socket_fd = setup_listening_socket(12345);
    server_loop(socket_fd);
}