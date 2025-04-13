#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <signal.h>
#include <time.h>

#define TUN_DEVICE "/dev/net/tun"
#define SERVER_PORT 5555
#define BUFFER_SIZE 1500
#define MAX_CLIENTS 32
#define IP_POOL_START 0x0A080002 // 10.8.0.2
#define IP_POOL_END 0x0A0800FE   // 10.8.0.254

typedef struct {
    struct in_addr ip;
    int client_fd;
    time_t last_active;
} ClientInfo;

ClientInfo clients[MAX_CLIENTS];
uint32_t next_ip = IP_POOL_START;
int tun_fd;
int server_fd;

/* Функция очистки ресурсов */
void cleanup() {
    printf("\nОчистка ресурсов...\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client_fd > 0) {
            close(clients[i].client_fd);
        }
    }
    system("ip link delete tun0 2>/dev/null");
    close(tun_fd);
    close(server_fd);
    exit(0);
}

/* Обработчик сигналов */
void handle_signal(int sig) {
    printf("\nПолучен сигнал %d\n", sig);
    cleanup();
}

/* Создание TUN интерфейса */
int tun_create(char *dev) {
    struct ifreq ifr;
    int fd;

    if ((fd = open(TUN_DEVICE, O_RDWR)) < 0) {
        perror("Ошибка открытия /dev/net/tun");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    if (*dev) {
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("Ошибка ioctl(TUNSETIFF)");
        close(fd);
        return -1;
    }

    printf("Создан TUN интерфейс: %s\n", ifr.ifr_name);
    return fd;
}

/* Назначение IP адреса */
void assign_ip_to_interface(const char *ip, const char *netmask) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev tun0", ip, netmask);
    system(cmd);
    system("ip link set tun0 up");
    printf("Назначен IP: %s/%s на tun0\n", ip, netmask);
}

/* Выдача IP клиенту */
struct in_addr assign_ip(int client_fd) {
    struct in_addr client_ip;
    
    // Поиск свободного IP
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client_fd == 0) {
            client_ip.s_addr = htonl(next_ip);
            clients[i].ip = client_ip;
            clients[i].client_fd = client_fd;
            clients[i].last_active = time(NULL);
            
            next_ip++;
            if (next_ip > IP_POOL_END) next_ip = IP_POOL_START;
            
            return client_ip;
        }
    }
    
    client_ip.s_addr = INADDR_NONE;
    return client_ip;
}

/* Освобождение IP */
void release_ip(int client_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client_fd == client_fd) {
            printf("Освобожден IP: %s\n", inet_ntoa(clients[i].ip));
            memset(&clients[i], 0, sizeof(ClientInfo));
            break;
        }
    }
}

/* Проверка неактивных клиентов */
void check_inactive_clients() {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client_fd != 0 && (now - clients[i].last_active) > 300) {
            printf("Клиент %s отключен по таймауту\n", inet_ntoa(clients[i].ip));
            close(clients[i].client_fd);
            release_ip(clients[i].client_fd);
        }
    }
}

/* Обработчик клиента */
void handle_client(int client_fd) {
    unsigned char buffer[BUFFER_SIZE];
    
    while (1) {
        int n = read(client_fd, buffer, BUFFER_SIZE);
        if (n <= 0) break;
        
        // Обновляем время активности
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].client_fd == client_fd) {
                clients[i].last_active = time(NULL);
                break;
            }
        }
        
        write(tun_fd, buffer, n);
        
        n = read(tun_fd, buffer, BUFFER_SIZE);
        if (n <= 0) break;
        
        write(client_fd, buffer, n);
    }
    
    close(client_fd);
    release_ip(client_fd);
}

/* Сервер */
void run_server() {
    struct sockaddr_in addr;
    int client_fd;
    
    // Настройка сервера
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);
    printf("Сервер слушает порт %d\n", SERVER_PORT);

    // Основной цикл
    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        // Выдача IP
        struct in_addr client_ip = assign_ip(client_fd);
        if (client_ip.s_addr == INADDR_NONE) {
            printf("Нет свободных IP!\n");
            close(client_fd);
            continue;
        }

        // Отправка IP клиенту
        send(client_fd, &client_ip, sizeof(client_ip), 0);
        printf("Клиент подключен. Выдан IP: %s\n", inet_ntoa(client_ip));

        // Запуск обработчика
        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);
            handle_client(client_fd);
            exit(0);
        }
        close(client_fd);
    }
}

/* Клиент */
void run_client(const char *server_ip) {
    struct sockaddr_in serv_addr;
    unsigned char buffer[BUFFER_SIZE];
    int sock_fd;
    
    // Подключение к серверу
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Ошибка подключения");
        cleanup();
    }

    // Получение IP от сервера
    struct in_addr my_ip;
    recv(sock_fd, &my_ip, sizeof(my_ip), 0);
    printf("Сервер выдал IP: %s\n", inet_ntoa(my_ip));

    // Настройка TUN
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip addr add %s/24 dev tun0", inet_ntoa(my_ip));
    system(cmd);
    system("ip link set tun0 up");

    // Основной цикл
    while (1) {
        int n = read(tun_fd, buffer, BUFFER_SIZE);
        if (n <= 0) break;
        write(sock_fd, buffer, n);
        
        n = read(sock_fd, buffer, BUFFER_SIZE);
        if (n <= 0) break;
        write(tun_fd, buffer, n);
    }
    
    close(sock_fd);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_signal);
    
    // Создание TUN
    tun_fd = tun_create("tun0");
    if (tun_fd < 0) return 1;

    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        assign_ip_to_interface("10.8.0.1", "24");
        run_server();
    } else if (argc > 2 && strcmp(argv[1], "client") == 0) {
        run_client(argv[2]);
    } else {
        printf("Использование:\n");
        printf("  Сервер: %s server\n", argv[0]);
        printf("  Клиент: %s client <IP_сервера>\n", argv[0]);
    }

    cleanup();
    return 0;
}
