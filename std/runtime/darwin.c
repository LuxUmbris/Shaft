typedef unsigned long long u64;
typedef long long i64;
typedef unsigned long usize;

i64 __shaft_entry(int argc, char **argv);
i64 __sys_close(i64 descriptor);
i64 __sys_send(i64 descriptor, const void *buffer, usize length, int flags);
i64 __sys_recv(i64 descriptor, void *buffer, usize length, int flags);

#if defined(__x86_64__)

static i64 syscall3(i64 number, i64 first, i64 second, i64 third)
{
    i64 result;
    __asm__ __volatile__("syscall" : "=a"(result) : "a"(number), "D"(first), "S"(second), "d"(third) : "rcx", "r11", "memory");
    return result;
}

#define SYS_EXIT 0x2000001
#define SYS_READ 0x2000003
#define SYS_WRITE 0x2000004
#define SYS_OPEN 0x2000005
#define SYS_CLOSE 0x2000006
#define SYS_LSEEK 0x20000c7
#define SYS_CLOCK_GETTIME 0x2000074
#define SYS_SOCKET     0x2000061
#define SYS_CONNECT    0x2000062
#define SYS_ACCEPT     0x200001e
#define SYS_BIND       0x2000068
#define SYS_LISTEN     0x200006a
#define SYS_SENDTO     0x2000097
#define SYS_RECVFROM   0x200001d
#define SYS_SHUTDOWN   0x200005a
#define SYS_FCNTL      0x200005c

static i64 syscall6(
    i64 number,
    i64 first,
    i64 second,
    i64 third,
    i64 fourth,
    i64 fifth,
    i64 sixth)
{
    i64 result;

    register i64 r10 __asm__("r10") = fourth;
    register i64 r8  __asm__("r8")  = fifth;
    register i64 r9  __asm__("r9")  = sixth;

    __asm__ __volatile__(
        "syscall"
        : "=a"(result)
        : "a"(number),
          "D"(first),
          "S"(second),
          "d"(third),
          "r"(r10),
          "r"(r8),
          "r"(r9)
        : "rcx", "r11", "memory"
    );

    return result;
}

#elif defined(__aarch64__)

static i64 syscall3(i64 number, i64 first, i64 second, i64 third)
{
    register i64 x0 __asm__("x0") = first;
    register i64 x1 __asm__("x1") = second;
    register i64 x2 __asm__("x2") = third;
    register i64 x16 __asm__("x16") = number;
    __asm__ __volatile__("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory");
    return x0;
}

#define SYS_EXIT 0x2000001
#define SYS_READ 0x2000003
#define SYS_WRITE 0x2000004
#define SYS_OPEN 0x2000005
#define SYS_CLOSE 0x2000006
#define SYS_LSEEK 0x20000c7
#define SYS_CLOCK_GETTIME 0x2000074
#define SYS_SOCKET     0x2000061
#define SYS_CONNECT    0x2000062
#define SYS_ACCEPT     0x200001e
#define SYS_BIND       0x2000068
#define SYS_LISTEN     0x200006a
#define SYS_SENDTO     0x2000097
#define SYS_RECVFROM   0x200001d
#define SYS_SHUTDOWN   0x200005a
#define SYS_FCNTL      0x200005c

static i64 syscall6(
    i64 number,
    i64 first,
    i64 second,
    i64 third,
    i64 fourth,
    i64 fifth,
    i64 sixth)
{
    register i64 x0 __asm__("x0") = first;
    register i64 x1 __asm__("x1") = second;
    register i64 x2 __asm__("x2") = third;
    register i64 x3 __asm__("x3") = fourth;
    register i64 x4 __asm__("x4") = fifth;
    register i64 x5 __asm__("x5") = sixth;
    register i64 x16 __asm__("x16") = number;

    __asm__ __volatile__(
        "svc #0x80"
        : "+r"(x0)
        : "r"(x1),
          "r"(x2),
          "r"(x3),
          "r"(x4),
          "r"(x5),
          "r"(x16)
        : "memory"
    );

    return x0;
}

#else
#error "Unsupported Darwin architecture"
#endif

void *memcpy(void *destination, const void *source, usize count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (usize index = 0; index < count; ++index)
        out[index] = in[index];
    return destination;
}

typedef struct shaft_alloc_block
{
    usize size;
    struct shaft_alloc_block *next;
    unsigned char is_free;
} shaft_alloc_block;

static union
{
    u64 alignment;
    unsigned char bytes[1024 * 1024];
} shaft_heap;
static usize shaft_heap_used;
static shaft_alloc_block *shaft_allocations;

void *__shaft_alloc(usize count)
{
    if (count > (usize)-1 - 7u)
        return 0;
    const usize aligned = (count + 7u) & ~7u;
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

void *memmove(void *destination, const void *source, usize count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    if (out <= in)
        return memcpy(destination, source, count);
    while (count > 0)
    {
        --count;
        out[count] = in[count];
    }
    return destination;
}

void *memset(void *destination, int value, usize count)
{
    unsigned char *out = destination;
    for (usize index = 0; index < count; ++index)
        out[index] = (unsigned char)value;
    return destination;
}

// Networking

#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_INET         2
#define AF_INET6        30

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

typedef unsigned char sa_family_t;

struct sockaddr
{
    unsigned char sa_len;
    sa_family_t sa_family;
    char sa_data[14];
};

struct in_addr
{
    unsigned int s_addr;
};

struct sockaddr_in
{
    unsigned char sin_len;
    sa_family_t sin_family;
    unsigned short sin_port;
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
    unsigned short sin6_port;
    unsigned int sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int sin6_scope_id;
};

i64 __sys_socket(int domain, int type, int protocol)
{
    return syscall3(SYS_SOCKET, domain, type, protocol);
}

i64 __sys_connect(i64 descriptor, const void *address, unsigned int address_length)
{
    return syscall3(SYS_CONNECT, descriptor, (i64)address, address_length);
}

i64 __sys_bind(i64 descriptor, const void *address, unsigned int address_length)
{
    return syscall3(SYS_BIND, descriptor, (i64)address, address_length);
}

i64 __sys_listen(i64 descriptor, int backlog)
{
    return syscall3(SYS_LISTEN, descriptor, backlog, 0);
}

i64 __sys_accept(i64 descriptor, void *address, unsigned int *address_length)
{
    return syscall3(SYS_ACCEPT, descriptor, (i64)address, (i64)address_length);
}

i64 __sys_shutdown(i64 descriptor, int how)
{
    return syscall3(SYS_SHUTDOWN, descriptor, how, 0);
}

i64 __sys_socket_close(i64 descriptor)
{
    return __sys_close(descriptor);
}

static unsigned short __net_htons(unsigned short value)
{
    return (unsigned short)(
        ((value & 0x00ffu) << 8) |
        ((value & 0xff00u) >> 8)
    );
}

static unsigned short __net_ntohs(unsigned short value)
{
    return __net_htons(value);
}

static unsigned int __net_htonl(unsigned int value)
{
#if defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)

    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8)  |
           ((value & 0x00ff0000u) >> 8)  |
           ((value & 0xff000000u) >> 24);

#else

    return value;

#endif
}

static unsigned int __net_ntohl(unsigned int value)
{
    return __net_htonl(value);
}

i64 __sys_sendto(
    i64 descriptor,
    const void *buffer,
    usize length,
    int flags,
    const struct sockaddr *destination,
    unsigned int destination_length)
{
    return syscall6(
        SYS_SENDTO,
        descriptor,
        (i64)buffer,
        (i64)length,
        (i64)flags,
        (i64)destination,
        (i64)destination_length
    );
}

i64 __sys_recvfrom(
    i64 descriptor,
    void *buffer,
    usize length,
    int flags,
    struct sockaddr *source,
    unsigned int *source_length)
{
    return syscall6(
        SYS_RECVFROM,
        descriptor,
        (i64)buffer,
        (i64)length,
        (i64)flags,
        (i64)source,
        (i64)source_length
    );
}

i64 __sys_tcp_socket(void)
{
    return __sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

i64 __sys_udp_socket(void)
{
    return __sys_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

i64 __sys_tcp_connect_ipv4(
    i64 descriptor,
    unsigned int ipv4,
    unsigned short port)
{
    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);

    return __sys_connect(
        descriptor,
        (const struct sockaddr *)&address,
        (unsigned int)sizeof(address)
    );
}

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 4
#define SHAFT_WOULD_BLOCK (-2)

i64 __sys_tcp_listen_ipv4(unsigned int ipv4, unsigned short port, int backlog)
{
    i64 descriptor = __sys_tcp_socket();
    if (descriptor < 0)
        return descriptor;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);
    if (__sys_bind(descriptor, &address, sizeof(address)) != 0 || __sys_listen(descriptor, backlog) != 0)
    {
        __sys_socket_close(descriptor);
        return -1;
    }
    return descriptor;
}

i64 __sys_socket_accept(i64 descriptor) { return __sys_accept(descriptor, 0, 0); }
i64 __sys_socket_set_nonblocking(i64 descriptor)
{
    i64 flags = syscall3(SYS_FCNTL, descriptor, F_GETFL, 0);
    return flags < 0 || syscall3(SYS_FCNTL, descriptor, F_SETFL, flags | O_NONBLOCK) != 0 ? -1 : 0;
}
static i64 __sys_normalize_would_block(i64 result)
{
    return result == 11 || result == 35 || result == -11 || result == -35 ? SHAFT_WOULD_BLOCK : result;
}
i64 __sys_socket_try_accept(i64 descriptor) { return __sys_normalize_would_block(__sys_socket_accept(descriptor)); }
i64 __sys_socket_try_send(i64 descriptor, const void *buffer, usize length) { return __sys_normalize_would_block(__sys_send(descriptor, buffer, length, 0)); }
i64 __sys_socket_try_recv(i64 descriptor, void *buffer, usize length) { return __sys_normalize_would_block(__sys_recv(descriptor, buffer, length, 0)); }

i64 __sys_send(i64 descriptor, const void *buffer, usize length, int flags)
{
    return syscall6(
        SYS_SENDTO,
        descriptor,
        (i64)buffer,
        (i64)length,
        (i64)flags,
        0,
        0
    );
}

i64 __sys_recv(i64 descriptor, void *buffer, usize length, int flags)
{
    return syscall6(
        SYS_RECVFROM,
        descriptor,
        (i64)buffer,
        (i64)length,
        (i64)flags,
        0,
        0
    );
}

i64 __sys_write(i64 fd, const void *buffer, usize count)
{
    return syscall3(SYS_WRITE, fd, (i64)buffer, (i64)count);
}

i64 __sys_read(i64 descriptor, void *buffer, usize count)
{
    return syscall3(SYS_READ, descriptor, (i64)buffer, (i64)count);
}

#define SHAFT_STDIN_BUFFER_SIZE 4096
static unsigned char shaft_stdin_buffer[SHAFT_STDIN_BUFFER_SIZE];
static usize shaft_stdin_offset;
static usize shaft_stdin_length;

i64 __sys_readline(void *buffer, usize capacity)
{
    unsigned char *output = buffer;
    usize length = 0;
    while (length < capacity)
    {
        if (shaft_stdin_offset == shaft_stdin_length)
        {
            i64 received = __sys_read(0, shaft_stdin_buffer, sizeof(shaft_stdin_buffer));
            if (received <= 0)
                return length != 0 ? (i64)length : received;
            shaft_stdin_offset = 0;
            shaft_stdin_length = (usize)received;
        }
        unsigned char byte = shaft_stdin_buffer[shaft_stdin_offset++];
        if (byte == '\n')
            break;
        output[length++] = byte;
    }
    return (i64)length;
}

i64 __sys_open(const char *pathname, int mode)
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
    return syscall3(SYS_OPEN, (i64)pathname, flags, 0644);
}

i64 __sys_close(i64 descriptor) { return syscall3(SYS_CLOSE, descriptor, 0, 0); }

i64 __sys_file_size(i64 descriptor)
{
    const i64 size = syscall3(SYS_LSEEK, descriptor, 0, 2 /* SEEK_END */);
    if (size < 0)
        return size;
    const i64 reset = syscall3(SYS_LSEEK, descriptor, 0, 0 /* SEEK_SET */);
    return reset < 0 ? reset : size;
}

__attribute__((noreturn)) void __sys_exit(int status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {}
}

void *__shaft_alloc_or_exit(usize count)
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


__attribute__((noreturn)) void exit(int status) { __sys_exit(status); }

i64 __cstr_strlen(const char *text)
{
    i64 length = 0;
    while (text[length] != 0)
        ++length;
    return length;
}

int __sys_gettime_parts(i64 *seconds, i64 *nanoseconds)
{
    struct timespec
    {
        i64 seconds;
        i64 nanoseconds;
    } value;
    int result = (int)syscall3(SYS_CLOCK_GETTIME, 0, (i64)&value, 0);
    if (result == 0)
    {
        *seconds = value.seconds;
        *nanoseconds = value.nanoseconds;
    }
    return result;
}

__attribute__((noreturn)) void _start(void)
{
    u64 *stack;
#if defined(__x86_64__)
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(stack));
#elif defined(__aarch64__)
    __asm__ __volatile__("mov %0, sp" : "=r"(stack));
#endif
    __sys_exit((int)__shaft_entry((int)stack[0], (char **)&stack[1]));
}
