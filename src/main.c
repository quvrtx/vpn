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

#define TUN_DEVICE "/dev/net/tun"
#define SERVER_PORT 12345
#define BUFFER_SIZE 1500
#define SERVER_TUN_IP "10.8.0.1"
#define SUBNET_MASK "255.255.255.0"

int tun_fd;
int sock_fd;

/* Завершение работы */
void cleanup() {
    printf("\nОчистка ресурсов...\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip link delete tun0 2>/dev/null");
    system(cmd);
    close(tun_fd);
    close(sock_fd);
    exit(0);
}

/* Обработчик сигналов */
void handle_signal(int sig) {
    printf("\nПолучен сигнал %d (Ctrl+C)\n", sig);
    cleanup();
}

/* Создание TUN-интерфейса */
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

    printf("Создан TUN-интерфейс: %s\n", ifr.ifr_name);
    return fd;
}

/* Настройка IP-адреса */
void setup_interface(const char *ip, const char *netmask) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev tun0", ip, netmask);
    system(cmd);
    system("ip link set tun0 up");
    printf("Настроен IP: %s/%s на tun0\n", ip, netmask);
}

/* Клиент */
void run_client(char* ip_addr) {
    struct sockaddr_in serv_addr;
    unsigned char buffer[BUFFER_SIZE];

    // Подключение к серверу
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, ip_addr, &serv_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Ошибка подключения к серверу");
        cleanup();
    }
    printf("Подключено к серверу %s:%d\n", ip_addr, SERVER_PORT);

    // Основной цикл
    while (1) {
        int n = read(tun_fd, buffer, BUFFER_SIZE);
        if (n < 0) break;
        write(sock_fd, buffer, n);  // Отправка на сервер

        n = read(sock_fd, buffer, BUFFER_SIZE);
        if (n < 0) break;
        write(tun_fd, buffer, n);   // Получение от сервера
    }
}

/* Сервер */
void run_server() {
    struct sockaddr_in addr;
    int client_fd;
    unsigned char buffer[BUFFER_SIZE];

    // Настройка сервера
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sock_fd, 5);
    printf("Сервер слушает порт %d\n", SERVER_PORT);

    // Принятие подключения
    client_fd = accept(sock_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("Ошибка accept()");
        cleanup();
    }
    printf("Клиент подключен\n");

    // Основной цикл
    while (1) {
        int n = read(client_fd, buffer, BUFFER_SIZE);
        if (n < 0) break;
        write(tun_fd, buffer, n);   // Отправка в TUN

        n = read(tun_fd, buffer, BUFFER_SIZE);
        if (n < 0) break;
        write(client_fd, buffer, n); // Ответ клиенту
    }
    close(client_fd);
}

int main(int argc, char *argv[]) {
    // Обработка Ctrl+C
    signal(SIGINT, handle_signal);
    // Обработка kill
    signal(SIGTERM, handle_signal);

    // Создание TUN
    tun_fd = tun_create("tun0");
    if (tun_fd < 0) return 1;

    // Настройка IP
    if (argc > 2 && strcmp(argv[1], "server") == 0) {
        setup_interface(SERVER_TUN_IP, "24");
        run_server();
    } else {
        setup_interface(argv[2], "24");
        run_client(argv[3]);
    }

    cleanup();
    return 0;
}
