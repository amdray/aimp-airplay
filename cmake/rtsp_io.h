/* Windows RTSP transport: one deadline per request, cancellable by Engine shutdown.
 * Included only in the generated rtsp_client.c, after the platform headers. */
extern int airplay_raop_cancelled(void);
extern unsigned long long airplay_raop_deadline(void);
extern void airplay_raop_io_failure(const char*, int);
extern void airplay_raop_event(const char*, int);
extern void airplay_raop_io_bytes(int, int);

static bool airplay_io_wait(int fd, short events, ULONGLONG deadline) {
    while (!airplay_raop_cancelled()) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { airplay_raop_io_failure("timeout", 0); break; }
        struct pollfd descriptor = {fd, events, 0};
        int remaining = (int)(deadline - now);
        int result = poll(&descriptor, 1, remaining < 50 ? remaining : 50);
        if (result > 0 && (descriptor.revents & events)) return true;
        if (result < 0) { airplay_raop_io_failure("poll_error", WSAGetLastError()); break; }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            int error = 0, length = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&error, &length);
            airplay_raop_io_failure("socket_closed", error);
            break;
        }
    }
    shutdown(fd, SD_BOTH); // No subsequent request may reuse a partial response.
    return false;
}

static bool airplay_tcp_connect(int fd, struct in_addr host, uint16_t port) {
    airplay_raop_event("CONNECT", 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr = host;
    address.sin_port = htons(port);
    set_nonblock(fd);
    if (!connect(fd, (struct sockaddr*)&address, sizeof(address))) return true;
    int connect_error = WSAGetLastError();
    if (connect_error != WSAEWOULDBLOCK) {
        airplay_raop_io_failure("connect_error", connect_error); return false;
    }
    if (!airplay_io_wait(fd, POLLOUT, airplay_raop_deadline())) return false;
    int error = 0, length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&error, &length)) error = WSAGetLastError();
    if (error) { airplay_raop_io_failure("connect_error", error); return false; }
    return true;
}

static int airplay_send(int fd, const char* data, int length, ULONGLONG deadline) {
    int total = 0;
    while (total < length) {
        if (!airplay_io_wait(fd, POLLOUT, deadline)) return -1;
        int count = send(fd, data + total, length - total, 0);
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (count <= 0) {
            airplay_raop_io_failure("send_error", count == SOCKET_ERROR ? WSAGetLastError() : 0);
            shutdown(fd, SD_BOTH); return -1;
        }
        total += count;
        airplay_raop_io_bytes(count, 0);
    }
    return total;
}

static int airplay_recv_flags(int fd, char* data, int length, ULONGLONG deadline, int flags) {
    while (airplay_io_wait(fd, POLLIN, deadline)) {
        int count = recv(fd, data, length, flags);
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (count <= 0) {
            airplay_raop_io_failure(count == 0 ? "peer_closed" : "recv_error", count == SOCKET_ERROR ? WSAGetLastError() : 0);
            shutdown(fd, SD_BOTH);
        }
        if (count > 0 && !(flags & MSG_PEEK)) airplay_raop_io_bytes(0, count);
        return count;
    }
    return -1;
}

static int airplay_recv(int fd, char* data, int length, ULONGLONG deadline) {
    return airplay_recv_flags(fd, data, length, deadline, 0);
}

static int airplay_read_line(int fd, char* line, int capacity, ULONGLONG deadline) {
    int size = 0;
    while (size < capacity - 1) {
        char buffer[512];
        int count = airplay_recv_flags(fd, buffer, sizeof(buffer), deadline, MSG_PEEK);
        if (count <= 0) return -1;
        int consume = 0;
        while (consume < count && buffer[consume++] != '\n') {}
        if (consume > capacity - size - 1) consume = capacity - size - 1;
        count = airplay_recv(fd, buffer, consume, deadline);
        if (count <= 0) return -1;
        for (int i = 0; i < count; ++i) {
            if (buffer[i] == '\n') { line[size] = 0; return size; }
            if (buffer[i] != '\r') line[size++] = buffer[i];
        }
    }
    shutdown(fd, SD_BOTH);
    return -1;
}
