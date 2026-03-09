/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: Noah Lykins

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
// New macros for pipeline operation
#define WINDOW_SIZE 100
#define TIMEOUT 1000

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int epoll_fd;
    int socket_fd;

    // added server_addr, tx_cnt, and rx_cnt fields for each client thread
    struct sockaddr_in server_addr;
    long tx_cnt;
    long rx_cnt;
} client_thread_data_t;


void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE];

    data->tx_cnt = 0;
    data->rx_cnt = 0;

    socklen_t server_addr_len = sizeof(data->server_addr);

    // Register the socket in the epoll instance
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event);

    // Send initial window of packets
    for (int i = 0; i < WINDOW_SIZE; i++) {
        if (sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0, (struct sockaddr *)&data->server_addr, server_addr_len) < 0) {
            perror("Client send failed, exiting");
            close(data->epoll_fd);
            exit(EXIT_FAILURE);
        }
        data->tx_cnt++;
    }

    // Loop until we've gotten a received packet for every sent packet (or we've timed-out)
    while (data->rx_cnt < num_requests) {
        // Wait for the response
        int n_events;
        if ((n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, TIMEOUT)) < 0) {
            perror("Client epoll wait failed, exiting");
            exit(EXIT_FAILURE);
        }

        // Time-out check
        if (n_events == 0) {
            break;
        }

        // If we didn't timeout, receive incoming packets and send the next one
        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == data->socket_fd) {
                if (recvfrom(data->socket_fd, recv_buf, MESSAGE_SIZE, 0, NULL, NULL) < 0) {
                    perror("Client message reception failed, exiting");
                    exit(EXIT_FAILURE);
                }
                data->rx_cnt++;

                if (data->tx_cnt < num_requests) {
                    if (sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0, (struct sockaddr *)&data->server_addr, server_addr_len) < 0) {
                        perror("Client send failed, exiting");
                        close(data->epoll_fd);
                        exit(EXIT_FAILURE);
                    }
                    data->tx_cnt++;
                }
            }
        }
    }

    printf("Client Packets Lost: %ld\n", data->tx_cnt - data->rx_cnt);

    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    // Create client threads
    for (int i = 0; i < num_client_threads; i++) {
        if ((thread_data[i].epoll_fd = epoll_create1(0)) < 0) {
            perror("Client epoll creation failed, exiting");
            exit(EXIT_FAILURE);
        }

        if ((thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
            perror("Client socket creation failed, exiting");
            close(thread_data[i].epoll_fd);
            exit(EXIT_FAILURE);
        }

        // populate new server_addr field in each thread's data
        thread_data[i].server_addr = server_addr;
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    // Wait for threads to complete, accumulate tx,rx, and lost packet counts
    long total_tx_cnt = 0;
    long total_rx_cnt = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_tx_cnt += thread_data[i].tx_cnt;
        total_rx_cnt += thread_data[i].rx_cnt;
    }

    printf("Total Packets Lost: %ld\n", total_tx_cnt - total_rx_cnt);
}

void run_server() {
    int server_fd;
    if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Server socket creation failed, exiting");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    // UDP only needs bind, no listen or accept, one socket for everything
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Server socket binding failed, exiting");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    int epoll_fd;
    if ((epoll_fd = epoll_create1(0)) < 0) {
        perror("Server epoll creation failed, exiting");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("Server epoll connection failed, exiting");
        close(epoll_fd);
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {

        int n_events;
        if ((n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1)) < 0) {
            perror("Server epoll wait failed, exiting");
            close(epoll_fd);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < n_events; i++) {
            char buffer[MESSAGE_SIZE];
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);

            // receive data
            int n = recvfrom(server_fd, buffer, MESSAGE_SIZE, 0, (struct sockaddr *)&client_addr, &client_addr_len);

            // echo data back/catch error
            if (n > 0) {
                if (sendto(server_fd, buffer, MESSAGE_SIZE, 0, (struct sockaddr *)&client_addr, client_addr_len) < 0) {
                    perror("Server send failed, exiting");
                    close(epoll_fd);
                    exit(EXIT_FAILURE);
                }
            } else if (n < 0) {
                perror("Server receive failed");
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}