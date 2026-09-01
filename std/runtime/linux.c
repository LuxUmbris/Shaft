#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_lseek 8
#define SYS_exit 60
#define SYS_clock_gettime 228
#define SYS_socket 41
#define SYS_connect 42
#define SYS_accept 43
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_shutdown 48
#define SYS_bind 49
#define SYS_listen 50
#define SYS_fcntl 72

static inline int64_t syscall6(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3,
                               int64_t arg4, int64_t arg5, int64_t arg6)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = arg4;
    register int64_t r8 __asm__("r8") = arg5;
    register int64_t r9 __asm__("r9") = arg6;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(sys_nr), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall4(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3,
                               int64_t arg4)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = arg4;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(sys_nr), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
                         : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3)
{
    int64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(sys_nr), "D"(arg1), "S"(arg2), "d"(arg3)
                         : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall2(int64_t sys_nr, int64_t arg1, int64_t arg2)
{
    return syscall3(sys_nr, arg1, arg2, 0);
}

static inline int64_t syscall1(int64_t sys_nr, int64_t arg1)
{
    return syscall3(sys_nr, arg1, 0, 0);
}

#elif defined(__aarch64__)

#define SYS_openat 56
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_exit 93
#define SYS_clock_gettime 113
#define SYS_munmap 215
#define SYS_lseek 62
#define SYS_mmap 222
#define SYS_socket 198
#define SYS_bind 200
#define SYS_listen 201
#define SYS_accept 202
#define SYS_connect 203
#define SYS_sendto 206
#define SYS_recvfrom 207
#define SYS_shutdown 210
#define SYS_fcntl 25

#define AT_FDCWD -100

static inline int64_t syscall6(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3,
                               int64_t arg4, int64_t arg5, int64_t arg6)
{
    register int64_t x8 __asm__("x8") = sys_nr;
    register int64_t x0 __asm__("x0") = arg1;
    register int64_t x1 __asm__("x1") = arg2;
    register int64_t x2 __asm__("x2") = arg3;
    register int64_t x3 __asm__("x3") = arg4;
    register int64_t x4 __asm__("x4") = arg5;
    register int64_t x5 __asm__("x5") = arg6;

    __asm__ __volatile__("svc #0"
                         : "+r"(x0)
                         : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                         : "memory");
    return x0;
}

static inline int64_t syscall4(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3,
                               int64_t arg4)
{
    return syscall6(sys_nr, arg1, arg2, arg3, arg4, 0, 0);
}

static inline int64_t syscall3(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3)
{
    return syscall4(sys_nr, arg1, arg2, arg3, 0);
}

static inline int64_t syscall2(int64_t sys_nr, int64_t arg1, int64_t arg2)
{
    return syscall3(sys_nr, arg1, arg2, 0);
}

static inline int64_t syscall1(int64_t sys_nr, int64_t arg1)
{
    return syscall3(sys_nr, arg1, 0, 0);
}

#elif defined(__riscv) && (__riscv_xlen == 64)

#define SYS_openat 56
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_exit 93
#define SYS_clock_gettime 113
#define SYS_munmap 215
#define SYS_lseek 62
#define SYS_mmap 222
#define SYS_socket 198
#define SYS_bind 200
#define SYS_listen 201
#define SYS_accept 202
#define SYS_connect 203
#define SYS_sendto 206
#define SYS_recvfrom 207
#define SYS_shutdown 210
#define SYS_fcntl 25

#define AT_FDCWD -100

static inline int64_t syscall6(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3,
                               int64_t arg4, int64_t arg5, int64_t arg6)
{
    register int64_t a7 __asm__("a7") = sys_nr;
    register int64_t a0 __asm__("a0") = arg1;
    register int64_t a1 __asm__("a1") = arg2;
    register int64_t a2 __asm__("a2") = arg3;
    register int64_t a3 __asm__("a3") = arg4;
    register int64_t a4 __asm__("a4") = arg5;
    register int64_t a5 __asm__("a5") = arg6;

    __asm__ __volatile__("ecall"
                         : "+r"(a0)
                         : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
                         : "memory");
    return a0;
}

static inline int64_t syscall4(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3,
                               int64_t arg4)
{
    return syscall6(sys_nr, arg1, arg2, arg3, arg4, 0, 0);
}

static inline int64_t syscall3(int64_t sys_nr, int64_t arg1, int64_t arg2, int64_t arg3)
{
    return syscall4(sys_nr, arg1, arg2, arg3, 0);
}

static inline int64_t syscall2(int64_t sys_nr, int64_t arg1, int64_t arg2)
{
    return syscall3(sys_nr, arg1, arg2, 0);
}

static inline int64_t syscall1(int64_t sys_nr, int64_t arg1)
{
    return syscall3(sys_nr, arg1, 0, 0);
}

#else
#error "Unsupported Architecture"
#endif

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++)
    {
        d[i] = s[i];
    }
    return dest;
}

typedef struct shaft_alloc_block
{
    size_t size;
    size_t ref_count;
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
    if (count > SIZE_MAX - 7u)
        return NULL;
    const size_t aligned = (count + 7u) & ~7u;
    for (shaft_alloc_block *block = shaft_allocations; block; block = block->next)
    {
        if (block->is_free && block->size >= aligned)
        {
            block->is_free = 0;
            block->ref_count = 1;
            return block + 1;
        }
    }
    if (aligned > sizeof(shaft_heap.bytes) - shaft_heap_used ||
        sizeof(shaft_alloc_block) > sizeof(shaft_heap.bytes) - shaft_heap_used - aligned)
        return NULL;
    shaft_alloc_block *block = (shaft_alloc_block *)(shaft_heap.bytes + shaft_heap_used);
    block->size = aligned;
    block->ref_count = 1;
    block->next = shaft_allocations;
    block->is_free = 0;
    shaft_allocations = block;
    shaft_heap_used += sizeof(shaft_alloc_block) + aligned;
    return block + 1;
}

void __shaft_retain(void *pointer)
{
    if (!pointer)
        return;
    for (shaft_alloc_block *block = shaft_allocations; block; block = block->next)
    {
        if (pointer == (void *)(block + 1) && !block->is_free)
        {
            ++block->ref_count;
            return;
        }
    }
}

void __shaft_free(void *pointer)
{
    if (!pointer)
        return;
    for (shaft_alloc_block *block = shaft_allocations; block; block = block->next)
    {
        if (pointer == (void *)(block + 1) && !block->is_free)
        {
            if (--block->ref_count == 0)
                block->is_free = 1;
            return;
        }
    }
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s)
    {
        for (size_t i = 0; i < n; i++)
        {
            d[i] = s[i];
        }
    }
    else if (d > s)
    {
        for (size_t i = n; i > 0; i--)
        {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++)
    {
        p[i] = (unsigned char)c;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++)
    {
        if (p1[i] != p2[i])
        {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

// Memory Allocation Syscalls
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

void *__sys_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
    return (void *)syscall6(SYS_mmap, (int64_t)addr, (int64_t)length, (int64_t)prot, (int64_t)flags,
                            (int64_t)fd, offset);
}

int64_t __sys_munmap(void *addr, size_t length)
{
    return syscall2(SYS_munmap, (int64_t)addr, (int64_t)length);
}

// I/O Syscalls
int64_t __sys_write(int64_t fd, const void *buf, size_t count)
{
    return syscall3(SYS_write, fd, (int64_t)buf, (int64_t)count);
}

int64_t __sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_read, fd, (int64_t)buf, (int64_t)count);
}

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
            const int64_t received = __sys_read(0, shaft_stdin_buffer, sizeof(shaft_stdin_buffer));
            if (received <= 0)
                return length != 0 ? (int64_t)length : received;
            shaft_stdin_offset = 0;
            shaft_stdin_length = (size_t)received;
        }
        const unsigned char byte = shaft_stdin_buffer[shaft_stdin_offset++];
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
    case 0: flags = 0; break;                                      // read
    case 1: flags = 1 | 64 | 512; break;                           // write/create/truncate
    case 2: flags = 2; break;                                      // read/write
    case 3: flags = 1 | 64 | 1024; break;                          // write/create/append
    default: return -22;
    }
#if defined(__x86_64__)
    return syscall3(SYS_open, (int64_t)pathname, flags, 0644);
#else
    return syscall4(SYS_openat, AT_FDCWD, (int64_t)pathname, flags, 0644);
#endif
}

int64_t __sys_file_size(int fd)
{
    const int64_t size = syscall3(SYS_lseek, fd, 0, 2 /* SEEK_END */);
    if (size < 0)
        return size;
    const int64_t reset = syscall3(SYS_lseek, fd, 0, 0 /* SEEK_SET */);
    return reset < 0 ? reset : size;
}

int64_t __sys_close(int fd) { return syscall1(SYS_close, fd); }

void __sys_exit(int status)
{
    syscall1(SYS_exit, status);
    while (1)
        ;
}

// Networking Syscalls

#define AF_UNSPEC 0
#define AF_INET   2
#define AF_INET6  10

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3

#define IPPROTO_IP     0
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17

#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

typedef uint16_t sa_family_t;
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

struct in_addr
{
    in_addr_t s_addr;
};

struct sockaddr_in
{
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
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct sockaddr_storage
{
    sa_family_t ss_family;
    unsigned char __data[126];
};

static uint16_t __net_htons(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t __net_htonl(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
#else
    return value;
#endif
}

int64_t __sys_socket(int domain, int type, int protocol)
{
    return syscall3(SYS_socket, domain, type, protocol);
}

int64_t __sys_connect(int64_t descriptor, const void *address, uint32_t address_length)
{
    return syscall3(SYS_connect, descriptor, (int64_t)address, address_length);
}

int64_t __sys_bind(int64_t descriptor, const void *address, uint32_t address_length)
{
    return syscall3(SYS_bind, descriptor, (int64_t)address, address_length);
}

int64_t __sys_listen(int64_t descriptor, int backlog)
{
    return syscall2(SYS_listen, descriptor, backlog);
}

int64_t __sys_accept(int64_t descriptor, void *address, uint32_t *address_length)
{
    return syscall3(SYS_accept, descriptor, (int64_t)address, (int64_t)address_length);
}

int64_t __sys_sendto(
    int64_t descriptor, const void *buffer, size_t length, int flags,
    const void *destination, uint32_t destination_length)
{
    return syscall6(SYS_sendto, descriptor, (int64_t)buffer, (int64_t)length,
                    flags, (int64_t)destination, destination_length);
}

int64_t __sys_recvfrom(
    int64_t descriptor, void *buffer, size_t length, int flags,
    void *source, uint32_t *source_length)
{
    return syscall6(SYS_recvfrom, descriptor, (int64_t)buffer, (int64_t)length,
                    flags, (int64_t)source, (int64_t)source_length);
}

int64_t __sys_send(int64_t descriptor, const void *buffer, size_t length, int flags)
{
    return __sys_sendto(descriptor, buffer, length, flags, 0, 0);
}

int64_t __sys_recv(int64_t descriptor, void *buffer, size_t length, int flags)
{
    return __sys_recvfrom(descriptor, buffer, length, flags, 0, 0);
}

int64_t __sys_shutdown(int64_t descriptor, int how)
{
    return syscall2(SYS_shutdown, descriptor, how);
}

int64_t __sys_socket_close(int64_t descriptor)
{
    return __sys_close((int)descriptor);
}

int64_t __sys_tcp_socket(void)
{
    return __sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

int64_t __sys_udp_socket(void)
{
    return __sys_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

int64_t __sys_tcp_connect_ipv4(int64_t descriptor, uint32_t ipv4, uint16_t port)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);
    return __sys_connect(descriptor, &address, sizeof(address));
}

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 04000
#define SHAFT_WOULD_BLOCK (-2)

int64_t __sys_tcp_listen_ipv4(uint32_t ipv4, uint16_t port, int backlog)
{
    const int64_t descriptor = __sys_tcp_socket();
    if (descriptor < 0)
        return descriptor;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);
    if (__sys_bind(descriptor, &address, sizeof(address)) != 0 ||
        __sys_listen(descriptor, backlog) != 0)
    {
        __sys_socket_close(descriptor);
        return -1;
    }
    return descriptor;
}

int64_t __sys_socket_accept(int64_t descriptor)
{
    return __sys_accept(descriptor, 0, 0);
}

int64_t __sys_socket_set_nonblocking(int64_t descriptor)
{
    const int64_t flags = syscall3(SYS_fcntl, descriptor, F_GETFL, 0);
    if (flags < 0)
        return flags;
    return syscall3(SYS_fcntl, descriptor, F_SETFL, flags | O_NONBLOCK);
}

static int64_t __sys_normalize_would_block(int64_t result)
{
    return result == -11 || result == -35 ? SHAFT_WOULD_BLOCK : result;
}

int64_t __sys_socket_try_accept(int64_t descriptor)
{
    return __sys_normalize_would_block(__sys_socket_accept(descriptor));
}

int64_t __sys_socket_try_send(int64_t descriptor, const void *buffer, size_t length)
{
    return __sys_normalize_would_block(__sys_send(descriptor, buffer, length, 0));
}

int64_t __sys_socket_try_recv(int64_t descriptor, void *buffer, size_t length)
{
    return __sys_normalize_would_block(__sys_recv(descriptor, buffer, length, 0));
}

void *__shaft_alloc_or_exit(size_t count)
{
    void *result = __shaft_alloc(count);
    if (!result && count != 0)
        __sys_exit(70);
    return result;
}

static size_t __shaft_decimal_u64(uint64_t value, char *output)
{
    char reversed[20];
    size_t length = 0;
    do
    {
        reversed[length] = (char)('0' + (value % 10));
        value /= 10;
        ++length;
    } while (value != 0);
    for (size_t index = 0; index < length; ++index)
        output[index] = reversed[length - index - 1];
    return length;
}

uint64_t __sys_int_to_string(int64_t value, char *output, uint64_t capacity)
{
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    char digits[20];
    const size_t digit_count = __shaft_decimal_u64(magnitude, digits);
    const uint64_t total = digit_count + (value < 0 ? 1 : 0);
    if (total > capacity)
        return 0;
    size_t offset = 0;
    if (value < 0)
        output[offset++] = '-';
    for (size_t index = 0; index < digit_count; ++index)
        output[offset + index] = digits[index];
    return total;
}

uint64_t __sys_uint_to_string(uint64_t value, char *output, uint64_t capacity)
{
    char digits[20];
    const size_t length = __shaft_decimal_u64(value, digits);
    if (length > capacity)
        return 0;
    for (size_t index = 0; index < length; ++index)
        output[index] = digits[index];
    return length;
}

uint64_t __sys_float_to_string(double value, char *output, uint64_t capacity)
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
    uint64_t whole = (uint64_t)magnitude;
    uint64_t fraction = (uint64_t)((magnitude - (double)whole) * 1000000.0 + 0.5);
    if (fraction == 1000000)
    {
        ++whole;
        fraction = 0;
    }
    char whole_digits[20];
    const size_t whole_count = __shaft_decimal_u64(whole, whole_digits);
    const uint64_t total = whole_count + (negative ? 1 : 0) + 7;
    if (total > capacity)
        return 0;
    size_t offset = 0;
    if (negative)
        output[offset++] = '-';
    for (size_t index = 0; index < whole_count; ++index)
        output[offset + index] = whole_digits[index];
    offset += whole_count;
    output[offset++] = '.';
    for (size_t divisor = 100000; divisor != 0; divisor /= 10)
        output[offset++] = (char)('0' + ((fraction / divisor) % 10));
    return total;
}

#if !defined(SHAFT_HOSTED)
void exit(int status)
{
    __sys_exit(status);
    __builtin_unreachable();
}
#endif

struct timespec
{
    int64_t tv_sec;  // seconds
    int64_t tv_nsec; // nanoseconds
};

int __sys_gettime(struct timespec *ts)
{
    // 0 = CLOCK_REALTIME
    return (int)syscall2(SYS_clock_gettime, 0, (int64_t)ts);
}

int __sys_gettime_parts(int64_t *seconds, int64_t *nanoseconds)
{
    struct timespec value;
    int result = __sys_gettime(&value);
    if (result == 0)
    {
        *seconds = value.tv_sec;
        *nanoseconds = value.tv_nsec;
    }
    return result;
}

size_t __cstr_strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

static void print_string(const char *str) { __sys_write(1 /* stdout */, str, __cstr_strlen(str)); }

int __shaft_entry(int argc, char **argv);

#if defined(SHAFT_HOSTED)
int main(int argc, char **argv) { return __shaft_entry(argc, argv); }
#else
void __sys_exit(int status) __attribute__((noreturn));

__attribute__((noreturn, noinline, used)) static void shaft_start(uint64_t *stack)
{
    const int argc = (int)stack[0];
    char **argv = (char **)&stack[1];
    __sys_exit(__shaft_entry(argc, argv));
    __builtin_unreachable();
}

#if defined(__x86_64__)
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ __volatile__("mov %rsp, %rdi\n"
                         "and $-16, %rsp\n"
                         "call shaft_start\n"
                         "ud2\n");
}
#elif defined(__aarch64__)
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ __volatile__("mov x0, sp\n"
                         "bl shaft_start\n"
                         "brk #0\n");
}
#elif defined(__riscv) && (__riscv_xlen == 64)
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ __volatile__("mv a0, sp\n"
                         "andi sp, sp, -16\n"
                         "call shaft_start\n"
                         "ebreak\n");
}
#endif
#endif