#include <stddef.h>
#include <stdint.h>

extern int64_t write(int fd, const void *buf, size_t count);
extern int64_t read(int fd, void *buf, size_t count);
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern int64_t lseek(int fd, int64_t offset, int whence);
extern void exit(int status) __attribute__((noreturn));
extern void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
extern int munmap(void *addr, size_t length);

struct timespec
{
    int64_t tv_sec;  // seconds
    int64_t tv_nsec; // nanoseconds
};

extern int clock_gettime(int clock_id, struct timespec *tp);

extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memmove(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern int memcmp(const void *s1, const void *s2, size_t n);

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x1000

void *__sys_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
    return mmap(addr, length, prot, flags, fd, offset);
}

int64_t __sys_munmap(void *addr, size_t length) { return munmap(addr, length); }

typedef struct shaft_alloc_block
{
    size_t size;
    struct shaft_alloc_block *next;
    unsigned char is_free;
} shaft_alloc_block;

static union
{
    max_align_t alignment;
    unsigned char bytes[1024 * 1024];
} shaft_heap;
static size_t shaft_heap_used;
static shaft_alloc_block *shaft_allocations;

void *__shaft_alloc(size_t count)
{
    if (count > (size_t)-1 - 7u)
        return 0;
    const size_t aligned = (count + 7u) & ~7u;
    for (shaft_alloc_block *block = shaft_allocations; block; block = block->next)
    {
        if (block->is_free && block->size >= aligned)
        {
            block->is_free = 0;
            return block + 1;
        }
    }
    if (aligned > sizeof(shaft_heap.bytes) - shaft_heap_used ||
        sizeof(shaft_alloc_block) > sizeof(shaft_heap.bytes) - shaft_heap_used - aligned)
        return 0;
    shaft_alloc_block *block = (shaft_alloc_block *)(shaft_heap.bytes + shaft_heap_used);
    block->size = aligned;
    block->next = shaft_allocations;
    block->is_free = 0;
    shaft_allocations = block;
    shaft_heap_used += sizeof(shaft_alloc_block) + aligned;
    return block + 1;
}

void __shaft_free(void *pointer)
{
    if (!pointer)
        return;
    for (shaft_alloc_block *block = shaft_allocations; block; block = block->next)
    {
        if (pointer == (void *)(block + 1))
        {
            block->is_free = 1;
            return;
        }
    }
}

int64_t __sys_write(int64_t fd, const void *buf, size_t count) { return write(fd, buf, count); }

int64_t __sys_read(int fd, void *buf, size_t count) { return read(fd, buf, count); }

#define SHAFT_STDIN_BUFFER_SIZE 4096
static unsigned char shaft_stdin_buffer[SHAFT_STDIN_BUFFER_SIZE];
static size_t shaft_stdin_offset;
static size_t shaft_stdin_length;

int64_t __sys_readline(void *buffer, size_t capacity)
{
    unsigned char *output = buffer;
    size_t length = 0;
    while (length < capacity)
    {
        if (shaft_stdin_offset == shaft_stdin_length)
        {
            int64_t received = __sys_read(0, shaft_stdin_buffer, sizeof(shaft_stdin_buffer));
            if (received <= 0)
                return length != 0 ? (int64_t)length : received;
            shaft_stdin_offset = 0;
            shaft_stdin_length = (size_t)received;
        }
        unsigned char byte = shaft_stdin_buffer[shaft_stdin_offset++];
        if (byte == '\n')
            break;
        output[length++] = byte;
    }
    return (int64_t)length;
}

int64_t __sys_open(const char *pathname, int mode);
int64_t __sys_open_read(const char *pathname) { return __sys_open(pathname, 0); }

int64_t __sys_open(const char *pathname, int mode)
{
    int flags = 0;
    switch (mode)
    {
    case 0: flags = 0; break;                       // read
    case 1: flags = 1 | 0x0200 | 0x0400; break;     // write/create/truncate
    case 2: flags = 2; break;                       // read/write
    case 3: flags = 1 | 0x0200 | 0x0008; break;     // write/create/append
    default: return -22;
    }
    return open(pathname, flags, 0644);
}

int64_t __sys_close(int fd) { return close(fd); }

int64_t __sys_file_size(int fd)
{
    const int64_t size = lseek(fd, 0, 2 /* SEEK_END */);
    if (size < 0)
        return size;
    const int64_t reset = lseek(fd, 0, 0 /* SEEK_SET */);
    return reset < 0 ? reset : size;
}

void __sys_exit(int status) { exit(status); }

void *__shaft_alloc_or_exit(size_t count)
{
    void *result = __shaft_alloc(count);
    if (!result && count != 0)
        __sys_exit(70);
    return result;
}

static unsigned long long __shaft_decimal_u64(unsigned long long value, char *output)
{
    char reversed[20];
    unsigned long long length = 0;
    do
    {
        reversed[length] = (char)('0' + (value % 10));
        value /= 10;
        ++length;
    } while (value != 0);
    for (unsigned long long index = 0; index < length; ++index)
        output[index] = reversed[length - index - 1];
    return length;
}

unsigned long long __sys_int_to_string(long long value, char *output, unsigned long long capacity)
{
    unsigned long long magnitude = value < 0 ? (unsigned long long)(-(value + 1)) + 1 : (unsigned long long)value;
    char digits[20];
    const unsigned long long digit_count = __shaft_decimal_u64(magnitude, digits);
    const unsigned long long total = digit_count + (value < 0 ? 1 : 0);
    if (total > capacity)
        return 0;
    unsigned long long offset = 0;
    if (value < 0)
        output[offset++] = '-';
    for (unsigned long long index = 0; index < digit_count; ++index)
        output[offset + index] = digits[index];
    return total;
}

unsigned long long __sys_uint_to_string(unsigned long long value, char *output, unsigned long long capacity)
{
    char digits[20];
    const unsigned long long length = __shaft_decimal_u64(value, digits);
    if (length > capacity)
        return 0;
    for (unsigned long long index = 0; index < length; ++index)
        output[index] = digits[index];
    return length;
}

unsigned long long __sys_float_to_string(double value, char *output, unsigned long long capacity)
{
    if (value != value)
    {
        if (capacity < 3)
            return 0;
        output[0] = 'n'; output[1] = 'a'; output[2] = 'n';
        return 3;
    }
    const int negative = value < 0;
    double magnitude = negative ? -value : value;
    if (magnitude > 9223372036854774784.0)
        return 0;
    unsigned long long whole = (unsigned long long)magnitude;
    unsigned long long fraction = (unsigned long long)((magnitude - (double)whole) * 1000000.0 + 0.5);
    if (fraction == 1000000)
    {
        ++whole;
        fraction = 0;
    }
    char whole_digits[20];
    const unsigned long long whole_count = __shaft_decimal_u64(whole, whole_digits);
    const unsigned long long total = whole_count + (negative ? 1 : 0) + 7;
    if (total > capacity)
        return 0;
    unsigned long long offset = 0;
    if (negative)
        output[offset++] = '-';
    for (unsigned long long index = 0; index < whole_count; ++index)
        output[offset + index] = whole_digits[index];
    offset += whole_count;
    output[offset++] = '.';
    for (unsigned long long divisor = 100000; divisor != 0; divisor /= 10)
        output[offset++] = (char)('0' + ((fraction / divisor) % 10));
    return total;
}


int __sys_gettime(struct timespec *ts)
{
    // 0 = CLOCK_REALTIME auf macOS
    return clock_gettime(0, ts);
}

size_t __cstr_strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

static void print_string(const char *str) { __sys_write(1 /* stdout */, str, __cstr_strlen(str)); }

// Networking

typedef unsigned char sa_family_t;
typedef unsigned short in_port_t;
typedef unsigned int in_addr_t;

#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6    30

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

struct sockaddr
{
    unsigned char sa_len;
    sa_family_t sa_family;
    char sa_data[14];
};

struct in_addr
{
    in_addr_t s_addr;
};

struct sockaddr_in
{
    unsigned char sin_len;
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct in6_addr
{
    unsigned char s6_addr[16];
};

struct sockaddr_in6
{
    unsigned char sin6_len;
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

extern int socket(int domain, int type, int protocol);

extern int connect(
    int socket_fd,
    const struct sockaddr *address,
    unsigned int address_length
);

extern int bind(
    int socket_fd,
    const struct sockaddr *address,
    unsigned int address_length
);

extern int listen(int socket_fd, int backlog);

extern int accept(
    int socket_fd,
    struct sockaddr *address,
    unsigned int *address_length
);

extern int64_t send(
    int socket_fd,
    const void *buffer,
    size_t length,
    int flags
);

extern int64_t recv(
    int socket_fd,
    void *buffer,
    size_t length,
    int flags
);

extern int64_t sendto(
    int socket_fd,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *destination,
    unsigned int destination_length
);

extern int64_t recvfrom(
    int socket_fd,
    void *buffer,
    size_t length,
    int flags,
    struct sockaddr *source,
    unsigned int *source_length
);

extern int shutdown(int socket_fd, int how);

int64_t __sys_socket(int domain, int type, int protocol)
{
    return socket(domain, type, protocol);
}

int64_t __sys_connect(
    int64_t socket_fd,
    const struct sockaddr *address,
    unsigned int address_length)
{
    return connect(
        (int)socket_fd,
        address,
        address_length
    );
}

int64_t __sys_bind(
    int64_t socket_fd,
    const struct sockaddr *address,
    unsigned int address_length)
{
    return bind(
        (int)socket_fd,
        address,
        address_length
    );
}

int64_t __sys_listen(int64_t socket_fd, int backlog)
{
    return listen((int)socket_fd, backlog);
}

int64_t __sys_accept(
    int64_t socket_fd,
    struct sockaddr *address,
    unsigned int *address_length)
{
    return accept(
        (int)socket_fd,
        address,
        address_length
    );
}

int64_t __sys_send(
    int64_t socket_fd,
    const void *buffer,
    size_t length,
    int flags)
{
    return send(
        (int)socket_fd,
        buffer,
        length,
        flags
    );
}

int64_t __sys_recv(
    int64_t socket_fd,
    void *buffer,
    size_t length,
    int flags)
{
    return recv(
        (int)socket_fd,
        buffer,
        length,
        flags
    );
}

int64_t __sys_sendto(
    int64_t socket_fd,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *destination,
    unsigned int destination_length)
{
    return sendto(
        (int)socket_fd,
        buffer,
        length,
        flags,
        destination,
        destination_length
    );
}

int64_t __sys_recvfrom(
    int64_t socket_fd,
    void *buffer,
    size_t length,
    int flags,
    struct sockaddr *source,
    unsigned int *source_length)
{
    return recvfrom(
        (int)socket_fd,
        buffer,
        length,
        flags,
        source,
        source_length
    );
}

int64_t __sys_shutdown(int64_t socket_fd, int how)
{
    return shutdown((int)socket_fd, how);
}

int64_t __sys_socket_close(int64_t socket_fd)
{
    return close((int)socket_fd);
}

static uint16_t __net_htons(uint16_t value)
{
    return (uint16_t)(
        ((value & 0x00ffu) << 8) |
        ((value & 0xff00u) >> 8)
    );
}

static uint16_t __net_ntohs(uint16_t value)
{
    return __net_htons(value);
}

static uint32_t __net_htonl(uint32_t value)
{
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8)  |
           ((value & 0x00ff0000u) >> 8)  |
           ((value & 0xff000000u) >> 24);
}

static uint32_t __net_ntohl(uint32_t value)
{
    return __net_htonl(value);
}

int64_t __sys_tcp_socket(void)
{
    return __sys_socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );
}

int64_t __sys_udp_socket(void)
{
    return __sys_socket(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP
    );
}

int64_t __sys_tcp_connect_ipv4(
    int64_t socket_fd,
    uint32_t ipv4,
    uint16_t port)
{
    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);

    return __sys_connect(
        socket_fd,
        (const struct sockaddr *)&address,
        (unsigned int)sizeof(address)
    );
}

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 4
#define SHAFT_WOULD_BLOCK (-2)
extern int fcntl(int fd, int command, ...);
extern int *__error(void);

int64_t __sys_tcp_listen_ipv4(uint32_t ipv4, uint16_t port, int backlog)
{
    int64_t descriptor = __sys_tcp_socket();
    if (descriptor < 0)
        return descriptor;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);
    if (__sys_bind(descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0 || __sys_listen(descriptor, backlog) != 0)
    {
        __sys_socket_close(descriptor);
        return -1;
    }
    return descriptor;
}

int64_t __sys_socket_accept(int64_t descriptor) { return __sys_accept(descriptor, 0, 0); }
int64_t __sys_socket_set_nonblocking(int64_t descriptor)
{
    int flags = fcntl((int)descriptor, F_GETFL);
    return flags < 0 || fcntl((int)descriptor, F_SETFL, flags | O_NONBLOCK) != 0 ? -1 : 0;
}
static int64_t __sys_normalize_would_block(int64_t result)
{
    return result == -1 && (*__error() == 11 || *__error() == 35) ? SHAFT_WOULD_BLOCK : result;
}
int64_t __sys_socket_try_accept(int64_t descriptor) { return __sys_normalize_would_block(__sys_socket_accept(descriptor)); }
int64_t __sys_socket_try_send(int64_t descriptor, const void *buffer, size_t length) { return __sys_normalize_would_block(__sys_send(descriptor, buffer, length, 0)); }
int64_t __sys_socket_try_recv(int64_t descriptor, void *buffer, size_t length) { return __sys_normalize_would_block(__sys_recv(descriptor, buffer, length, 0)); }

int __shaft_entry(int argc, char **argv);

int main(int argc, char **argv)
{
    int status = __shaft_entry(argc, argv);
    __sys_exit(status);
    return status;
}