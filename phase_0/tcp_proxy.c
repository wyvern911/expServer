#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>

#define PORT 8080
#define BUFF_SIZE 10000
#define MAX_ACCEPT_BACKLOG 5
#define MAX_EPOLL_EVENTS 10
#define UPSTREAM_PORT 3000
#define MAX_SOCKS 10

int listen_sock_fd, epoll_fd;
struct epoll_event events[MAX_EPOLL_EVENTS];
int route_table[MAX_SOCKS][2], route_table_size=0;  

// Check whether FD is client socket
int is_client(int fd) {
    for (int i=0; i<route_table_size; i++) {
        if (fd==route_table[i][0]) return 1;
    }
    return 0;
}

// Check whether FD is upstream socket
int is_upstream(int fd) {
    for (int i=0; i<route_table_size; i++) {
        if (fd==route_table[i][1]) return 1;
    }
    return 0;
}

// Route table helper routine 1
int get_upstream_by_client(int conn_sock_fd) {
    for (int i=0; i<route_table_size; i++) {
        if (route_table[i][0]==conn_sock_fd) return route_table[i][1];
    }
    return -1;
}

// Route table helper routine 2
int get_client_by_upstream(int upstream_sock_fd) {
    for (int i=0; i<route_table_size; i++) {
        if (route_table[i][1]==upstream_sock_fd) return route_table[i][0];
    }
    return -1;
}

// Cleanup function
void cleanup_connection(int fd) {
   int client_fd=-1, upstream_fd=-1, idx=-1;
    
    // Find the pair in the routing table
    for (int i=0; i<route_table_size; i++) {
        if (route_table[i][0]==fd || route_table[i][1]==fd) {
            client_fd=route_table[i][0];
            upstream_fd=route_table[i][1];
            idx=i;
            break;
        }
    }

    if (idx!=-1) {
        // Remove from epoll
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, upstream_fd, NULL);
        
        // Close sockets
        close(client_fd);
        close(upstream_fd);

        // Remove from route table by replacing with the last element
        route_table[idx][0]=route_table[route_table_size-1][0];
        route_table[idx][1]=route_table[route_table_size-1][1];
        route_table_size--;
    }
}

// Create corresponding connection to the upstream server.
int connect_upstream() {
    int fd=socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in upstream_addr;
    upstream_addr.sin_family=AF_INET;
    upstream_addr.sin_addr.s_addr=inet_addr("127.0.0.1");
    upstream_addr.sin_port=htons(UPSTREAM_PORT);

    connect(fd, (struct sockaddr* ) &upstream_addr, sizeof(upstream_addr));

    return fd;
}

// Prototype for accept_connection
void loop_attach(int epoll_fd, int fd, int events);

// Accept client connection
void accept_connection(int listen_sock_fd) {
    // Client address object
    struct sockaddr_in client_addr;
    socklen_t client_addr_len=sizeof(client_addr);

    // Accept client connection
    int conn_sock_fd=accept(listen_sock_fd, (struct sockaddr *) &client_addr, &client_addr_len);
    printf("[INFO] Client connected to server\n");

    // Add conn_sock_fd to loop
    loop_attach(epoll_fd, conn_sock_fd, EPOLLIN);

    // Create corresponding upstream connection socket
    int upstream_sock_fd=connect_upstream();

    // Add upstream_sock_fd to loop
    loop_attach(epoll_fd, upstream_sock_fd, EPOLLIN);

    // Add both to routing table
    if (route_table_size<MAX_SOCKS) {
        route_table[route_table_size][0]=conn_sock_fd;
        route_table[route_table_size][1]=upstream_sock_fd;
        route_table_size+=1;
    } else {
        printf("[ERROR] Route table full. Dropping connection\n");
        close(conn_sock_fd);
        close(upstream_sock_fd);
    }
}

// Routine to handle client
void handle_client(int conn_sock_fd) {
    char buff[BUFF_SIZE];
    memset(buff, 0 ,BUFF_SIZE);

    // Read message from client to buffer
    ssize_t read_n=recv(conn_sock_fd, buff, sizeof(buff), 0);

    // Client disconnected or error occured
    if (read_n<=0) {
        printf("[INFO] Client disconnected\n");
        cleanup_connection(conn_sock_fd);
        return;
    }

    printf("[CLIENT MESSAGE] %s", buff);

    // Find the corresponding upstream socket from route table
    int upstream_sock_fd=get_upstream_by_client(conn_sock_fd);
    if (upstream_sock_fd==-1) return;

    // Sending client message upstream
    int bytes_written=0;
    int message_len=read_n;

    while (bytes_written<message_len) {
        int n=send(upstream_sock_fd, buff+bytes_written, message_len-bytes_written, 0);
        if (n<=0) break;
        bytes_written+=n;
    }
}

// Routine to relay client to upstream
void handle_upstream(int upstream_sock_fd) {
    char buff[BUFF_SIZE];
    memset(buff, 0 ,BUFF_SIZE);

    // Read message from upstream server to buffer
    ssize_t read_n=recv(upstream_sock_fd, buff, sizeof(buff), 0);

    // Upstream server disconnected or error occured
    if (read_n<=0) {
        printf("[INFO] Upstream server disconnected\n");
        cleanup_connection(upstream_sock_fd);
        return;
    }

    // Find the corresponding client socket from route table
    int conn_sock_fd=get_client_by_upstream(upstream_sock_fd);
    if (conn_sock_fd==-1) return;

    // Sending client message upstream
    int bytes_written=0;
    int message_len=read_n;

    while (bytes_written<message_len) {
        int n=send(conn_sock_fd, buff+bytes_written, message_len-bytes_written, 0);
        if (n<=0) break;
        bytes_written+=n;
    }
}

// Return new epoll instance
int create_loop() {
    return epoll_create1(0);
}

// Attach fd to epoll
void loop_attach(int epoll_fd, int fd, int events) {
    struct epoll_event event;
    event.events=events;
    event.data.fd=fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

// Create listening socket and return it
int create_server() {
    // Create listening socket
    int fd=socket(AF_INET, SOCK_STREAM, 0);

    // Resuse address
    int enable=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

    // Creating an object of struct socketaddr_in
    struct sockaddr_in server_addr;

    // Setting up server addr
    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=htonl(INADDR_ANY);
    server_addr.sin_port=htons(PORT);

    // Binding listening sock to port
    bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    // Starting to listen
    listen(fd, MAX_ACCEPT_BACKLOG);
    printf("[INFO] Server listening on port %d\n", PORT);

    return fd;
}

// Infinite loop and processing epoll events
void loop_run(int epoll_fd) {
    while (1) {
        printf("[DEBUG] Epoll wait\n");

        // Block until some event occurs on the FDs in the interset list
        int n_ready_fds=epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);

        // Iterate through FDs in ready list
        for (int i=0; i<n_ready_fds; i++) {
            int curr_fd=events[i].data.fd;

            // Event is in listening socket
            if (curr_fd==listen_sock_fd) {
                accept_connection(curr_fd);
            }

            // Event is in client connection socket
            else if (is_client(curr_fd)) {
                handle_client(curr_fd);
            }

            // Event is in upstream server connection socket
            else if (is_upstream(curr_fd)){
                handle_upstream(curr_fd);
            }

        }
    }
}

int main() {
    // Create server
    listen_sock_fd=create_server();

    // Create epoll instance
    epoll_fd=create_loop();

    // Attach server to event loop
    loop_attach(epoll_fd, listen_sock_fd, EPOLLIN);

    // Start event loop
    loop_run(epoll_fd);

    // Close all the FDs
    close(listen_sock_fd);
    close(epoll_fd);

    return 0;
}