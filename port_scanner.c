#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     
#include <getopt.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>

typedef struct {
    char ip_str[INET_ADDRSTRLEN];
    int port;
    struct Task* next;
} Task;

Task* task_queue_head = NULL;
Task* task_queue_tail = NULL;
pthread_mutex_t queue_lock;
pthread_mutex_t printf_lock;
int global_timeout_ms = 1000;
int udp_scan = 0;

void print_usage(const char* prog_name);
void add_task(const char* ip, int port);
void* worker_thread(void* arg);
void probe_port(const char* ip, int port);
void probe_port_udp(const char* ip, int port);

int main(int argc, char* argv[]) {
    if (argc == 1) {
        print_usage(argv[0]);
        return 1;
    }

    char* target_spec = NULL;
    char* port_spec = NULL;
    int num_jobs = 4;

    struct option long_options[] = {
        {"help",    no_argument,       0, 'h'},
        {"targets", required_argument, 0, 't'},
        {"ports",   required_argument, 0, 'p'},
        {"jobs",    required_argument, 0, 'j'},
        {"timeout", required_argument, 0, 'w'},
        {"udp",     no_argument,       0, 'u'},
        {0, 0, 0, 0}
    };

    int opt;
    
    while ((opt = getopt_long(argc, argv, "ht:p:j:w:u", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 't':
                target_spec = optarg;
                break;
            case 'p':
                port_spec = optarg;
                break;
            case 'j':
                num_jobs = atoi(optarg);
                if (num_jobs <= 0) {
                    num_jobs = 1;
                }
                break;
            case 'w':
                global_timeout_ms = atoi(optarg);
                if (global_timeout_ms <= 0) {
                    global_timeout_ms = 1000;
                }
                break;
            case 'u':
                udp_scan = 1;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (target_spec == NULL || port_spec == NULL) {
        fprintf(stderr, "Error: Both targets (-t) and ports (-p) are required.\n");
        print_usage(argv[0]);
        return 1;
    }

    int start_port = 0, end_port = 0;
    char* dash = strchr(port_spec, '-');
    if (dash) {
        *dash = '\0';
        start_port = atoi(port_spec);
        end_port = atoi(dash + 1);
    } else {
        start_port = end_port = atoi(port_spec);
    }

    if (start_port <= 0 || end_port <= 0 || end_port < start_port) {
        fprintf(stderr, "Error: Invalid port specification.\n");
        return 1;
    }
    
    pthread_mutex_init(&queue_lock, NULL);
    pthread_mutex_init(&printf_lock, NULL);

    dash = strchr(target_spec, '-');
    if (dash) {
        *dash = '\0';
        char* start_ip_str = target_spec;
        char* end_ip_str = dash + 1;

        struct in_addr start_addr, end_addr;
        if (!inet_aton(start_ip_str, &start_addr) || !inet_aton(end_ip_str, &end_addr)) {
            fprintf(stderr, "Error: Invalid IP address in range.\n");
            return 1;
        }
        
        uint32_t start_ip = ntohl(start_addr.s_addr);
        uint32_t end_ip = ntohl(end_addr.s_addr);

        if (start_ip > end_ip) {
            fprintf(stderr, "Error: Invalid IP range (start > end).\n");
            return 1;
        }
        
        for (uint32_t ip = start_ip; ip <= end_ip; ip++) {
            struct in_addr current_addr;
            current_addr.s_addr = htonl(ip);
            char* ip_str = inet_ntoa(current_addr);
            for (int port = start_port; port <= end_port; port++) {
                add_task(ip_str, port);
            }
        }
    } else {
        struct hostent *host = gethostbyname(target_spec);
        if (host == NULL) {
            fprintf(stderr, "Error: Cannot resolve target '%s'.\n", target_spec);
            return 1;
        }
        
        char* ip_str = inet_ntoa(*(struct in_addr*)host->h_addr_list[0]);
        for (int port = start_port; port <= end_port; port++) {
            add_task(ip_str, port);
        }
    }

    pthread_t threads[num_jobs];
    for (int i = 0; i < num_jobs; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 0; i < num_jobs; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&queue_lock);
    pthread_mutex_destroy(&printf_lock);

    return 0;
}

void print_usage(const char* prog_name) {
    printf("Usage: %s [OPTIONS] -t <targets> -p <ports>\n", prog_name);
    printf("\nOptions:\n");
    printf("  -h, --help            Show this usage message.\n");
    printf("  -t, --targets <spec>  Target(s) to scan. Forms:\n");
    printf("                        - Single IP: 192.168.1.42\n");
    printf("                        - Hostname:  localhost\n");
    printf("                        - IP Range:  192.168.1.1-192.168.1.255\n");
    printf("  -p, --ports <spec>    Port(s) to scan. Forms:\n");
    printf("                        - Single Port: 22\n");
    printf("                        - Port Range:  1-1024\n");
    printf("  -j, --jobs <n>        Number of concurrent threads (default: 4).\n");
    printf("  -w, --timeout <ms>    Connection timeout in milliseconds (default: 1000).\n");
    printf("  -u, --udp             Use UDP scan mode (default: TCP).\n");
}

void add_task(const char* ip, int port) {
    Task* new_task = (Task*)malloc(sizeof(Task));
    if (!new_task) {
        perror("malloc");
        return;
    }
    strncpy(new_task->ip_str, ip, INET_ADDRSTRLEN);
    new_task->port = port;
    new_task->next = NULL;
    
    if (task_queue_tail == NULL) {
        task_queue_head = task_queue_tail = new_task;
    } else {
        task_queue_tail->next = new_task;
        task_queue_tail = new_task;
    }
}

void* worker_thread(void* arg) {
    (void)arg;
    
    while (1) {
        pthread_mutex_lock(&queue_lock);
        
        if (task_queue_head == NULL) {
            pthread_mutex_unlock(&queue_lock);
            break;
        }
        
        Task* task = task_queue_head;
        task_queue_head = task_queue_head->next;
        if (task_queue_head == NULL) {
            task_queue_tail = NULL;
        }
        
        pthread_mutex_unlock(&queue_lock);

        if (udp_scan) {
            probe_port_udp(task->ip_str, task->port);
        } else {
            probe_port(task->ip_str, task->port);
        }
        
        free(task);
    }
    return NULL;
}

void probe_port(const char* ip, int port) {
    int sockfd;
    struct sockaddr_in serv_addr;
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr(ip);
    
    long arg = fcntl(sockfd, F_GETFL, NULL);
    arg |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, arg);

    int connect_res = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    
    if (connect_res < 0) {
        if (errno == EINPROGRESS) {
            fd_set write_fds;
            struct timeval timeout;
            
            timeout.tv_sec = global_timeout_ms / 1000;
            timeout.tv_usec = (global_timeout_ms % 1000) * 1000;
            
            FD_ZERO(&write_fds);
            FD_SET(sockfd, &write_fds);

            int select_res = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
            
            if (select_res > 0) {
                int so_error;
                socklen_t len = sizeof(so_error);
                getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);

                if (so_error == 0) {
                    struct servent *service = getservbyport(htons(port), "tcp");
                    
                    pthread_mutex_lock(&printf_lock);
                    printf("%s:%d (%s) open\n", ip, port, service ? service->s_name : "unknown");
                    fflush(stdout);
                    pthread_mutex_unlock(&printf_lock);
                }
            }
        }
    } else {
        struct servent *service = getservbyport(htons(port), "tcp");
        pthread_mutex_lock(&printf_lock);
        printf("%s:%d (%s) open\n", ip, port, service ? service->s_name : "unknown");
        fflush(stdout);
        pthread_mutex_unlock(&printf_lock);
    } 
    
    close(sockfd);
}

void probe_port_udp(const char* ip, int port) {
    int sockfd;
    struct sockaddr_in serv_addr;
    
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr(ip);

    struct timeval timeout;
    timeout.tv_sec = global_timeout_ms / 1000;
    timeout.tv_usec = (global_timeout_ms % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
        close(sockfd);
        return;
    }

    if (sendto(sockfd, NULL, 0, 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return;
    }

    char buffer[1];
    if (recvfrom(sockfd, buffer, 0, 0, NULL, NULL) < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            struct servent *service = getservbyport(htons(port), "udp");
            pthread_mutex_lock(&printf_lock);
            printf("%s:%d (%s) open|filtered\n", ip, port, service ? service->s_name : "unknown");
            fflush(stdout);
            pthread_mutex_unlock(&printf_lock);
        } else if (errno == ECONNREFUSED) {}
    } else {
        struct servent *service = getservbyport(htons(port), "udp");
        pthread_mutex_lock(&printf_lock);
        printf("%s:%d (%s) open\n", ip, port, service ? service->s_name : "unknown");
        fflush(stdout);
        pthread_mutex_unlock(&printf_lock);
    }
    
    close(sockfd);
}
