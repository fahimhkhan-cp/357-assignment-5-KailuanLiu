#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#define BUFFER_SIZE 1024
#define RESPONSE_HEADER_SIZE 512

void handle_client(int client_socket);
void send_response(int client_socket, const char *status, const char *content_type, const char *body, size_t content_length);
void send_error(int client_socket, const char *status, const char *message);
void handle_signal(int sig);
void handle_file_request(int client_socket, const char *method, const char *path);
void handle_cgi_request(int client_socket, const char *path, const char *query);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port < 1024 || port > 65535) {
        fprintf(stderr, "Port must be between 1024 and 65535.\n");
        exit(EXIT_FAILURE);
    }

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Binding failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 10) < 0) {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d\n", port);

    signal(SIGCHLD, handle_signal);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            close(server_socket);
            handle_client(client_socket);
            close(client_socket);
            exit(EXIT_SUCCESS);
        } else if (pid > 0) {
            // Parent process
            close(client_socket);
        } else {
            perror("Fork failed");
        }
    }

    close(server_socket);
    return 0;
}

void handle_signal(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE] = {0};
    if (recv(client_socket, buffer, sizeof(buffer), 0) <= 0) {
        perror("Failed to read request");
        return;
    }

    char method[16], path[256], version[16];
    if (sscanf(buffer, "%15s %255s %15s", method, path, version) != 3) {
        send_error(client_socket, "400 Bad Request", "Malformed request line.");
        return;
    }

    if (strncmp(path, "/cgi-like/", 10) == 0) {
        char *query = strchr(path, '?');
        if (query) {
            *query = '\0'; // Split path and query
            query++;
        }
        handle_cgi_request(client_socket, path + 10, query);
    } else {
        handle_file_request(client_socket, method, path);
    }
}

void send_response(int client_socket, const char *status, const char *content_type, const char *body, size_t content_length) {
    char header[RESPONSE_HEADER_SIZE];
    snprintf(header, sizeof(header),
             "HTTP/1.0 %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "\r\n",
             status, content_type, content_length);
    send(client_socket, header, strlen(header), 0);
    if (body && content_length > 0) {
        send(client_socket, body, content_length, 0);
    }
}

void send_error(int client_socket, const char *status, const char *message) {
    char body[BUFFER_SIZE];
    snprintf(body, sizeof(body),
             "<html><body><h1>%s</h1><p>%s</p></body></html>",
             status, message);
    send_response(client_socket, status, "text/html", body, strlen(body));
}

void handle_file_request(int client_socket, const char *method, const char *path) {
    if (strstr(path, "..")) {
        send_error(client_socket, "403 Forbidden", "Access denied.");
        return;
    }

    char filepath[256];
    snprintf(filepath, sizeof(filepath), ".%s", path);

    struct stat file_stat;
    if (stat(filepath, &file_stat) < 0) {
        send_error(client_socket, "404 Not Found", "File not found.");
        return;
    }

    if (strcmp(method, "HEAD") == 0) {
        send_response(client_socket, "200 OK", "text/html", NULL, file_stat.st_size);
    } else if (strcmp(method, "GET") == 0) {
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            send_error(client_socket, "500 Internal Server Error", "Failed to open file.");
            return;
        }

        char *content = malloc(file_stat.st_size);
        read(fd, content, file_stat.st_size);
        close(fd);

        send_response(client_socket, "200 OK", "text/html", content, file_stat.st_size);
        free(content);
    } else {
        send_error(client_socket, "501 Not Implemented", "Method not supported.");
    }
}

void handle_cgi_request(int client_socket, const char *path, const char *query) {
    char cgi_path[256];
    snprintf(cgi_path, sizeof(cgi_path), "./cgi-like/%s", path);

    struct stat cgi_stat;
    if (stat(cgi_path, &cgi_stat) < 0 || access(cgi_path, X_OK) < 0) {
        send_error(client_socket, "404 Not Found", "CGI program not found or not executable.");
        return;
    }

    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        send_error(client_socket, "500 Internal Server Error", "Pipe creation failed.");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);

        if (query) {
            execl(cgi_path, cgi_path, query, NULL);
        } else {
            execl(cgi_path, cgi_path, NULL);
        }
        perror("exec failed");
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        // Parent process
        close(pipe_fd[1]);

        char output[BUFFER_SIZE];
        ssize_t bytes_read = read(pipe_fd[0], output, sizeof(output));
        close(pipe_fd[0]);

        if (bytes_read > 0) {
            send_response(client_socket, "200 OK", "text/html", output, bytes_read);
        } else {
            send_error(client_socket, "500 Internal Server Error", "CGI execution failed.");
        }

        waitpid(pid, NULL, 0);
    } else {
        send_error(client_socket, "500 Internal Server Error", "Fork failed.");
    }
}

