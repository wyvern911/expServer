#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 8080
#define BUFF_SIZE 10000

int main() {
    // Create client socket
    int client_sock_fd=socket(AF_INET, SOCK_STREAM, 0);

    // Create an object of struct socketaddr_in
    struct sockaddr_in server_addr;

    // Set up the server address
    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=inet_addr("127.0.0.1");
    server_addr.sin_port=htons(SERVER_PORT);

    // Connect to TCP server
    if (connect(client_sock_fd, (struct sockaddr *) &server_addr, sizeof(server_addr))!=0) {
        printf("[ERROR] Failed to connect to TCP server\n");
        exit(1);
    } else {
        printf("[INFO] Connected to TCP server\n");
    }

    // Loop to take input from user, send to server and receive from client
    while (1) {
        // Buffer
        char* line=NULL;
        size_t line_len=0;
        ssize_t read_n;

        // Read from user with error handling
        read_n=getline(&line, &line_len, stdin);
        if (read_n<=0) {
            printf("[ERROR] Reading failed\n");
            exit(1);
        }

        // Send message to tcp server
        send(client_sock_fd, line, read_n, 0);

        // Create a buffer to store server message and memset to 0
        char buff[BUFF_SIZE];
        memset(buff, 0, BUFF_SIZE);

        // Read message from server and store in buffer
        read_n=recv(client_sock_fd, buff, read_n, 0);

        // Server closed connection or error occurred
        if (read_n <= 0) {
            printf("[INFO] Server disconnected. Closing client\n");
            close(client_sock_fd);
            exit(1);
        }

        // Print the reversed input
        printf("[SERVER MESSAGE] %s", buff);

        // Avoid memory leaks
        free(line);

    }

    return 0;

}