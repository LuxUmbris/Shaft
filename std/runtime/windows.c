typedef unsigned long long u64;
typedef long long i64;
typedef unsigned long usize;
typedef unsigned long dword;
typedef int bool32;
typedef void *handle;

i64 __shaft_entry(int argc, char **argv);

__declspec(dllimport) void __stdcall ExitProcess(unsigned int code);
__declspec(dllimport) handle __stdcall GetStdHandle(dword standard_handle);
__declspec(dllimport) handle __stdcall CreateFileA(const char *path, dword desired_access, dword share_mode,
                                                    void *security_attributes, dword creation_disposition,
                                                    dword flags_and_attributes, handle template_file);
__declspec(dllimport) bool32 __stdcall ReadFile(handle file, void *buffer, dword count,
                                                 dword *read, void *overlapped);
__declspec(dllimport) bool32 __stdcall WriteFile(handle file, const void *buffer, dword count,
                                                  dword *written, void *overlapped);
__declspec(dllimport) bool32 __stdcall CloseHandle(handle object);
__declspec(dllimport) bool32 __stdcall GetFileSizeEx(handle file, i64 *size);
__declspec(dllimport) bool32 __stdcall SetFilePointerEx(handle file, i64 distance, i64 *new_position,
                                                         dword move_method);
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(void *file_time);

#define STD_INPUT_HANDLE ((dword)-10)
#define STD_OUTPUT_HANDLE ((dword)-11)
#define STD_ERROR_HANDLE ((dword)-12)
#define GENERIC_READ 0x80000000U
#define GENERIC_WRITE 0x40000000U
#define FILE_SHARE_READ 0x00000001U
#define FILE_SHARE_WRITE 0x00000002U
#define CREATE_ALWAYS 2U
#define OPEN_EXISTING 3U
#define OPEN_ALWAYS 4U
#define FILE_END 2U
#define INVALID_HANDLE_VALUE ((handle)(u64)-1)
#define WINDOWS_TICKS_PER_SECOND 10000000ULL
#define UNIX_EPOCH_TICKS 116444736000000000ULL

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

static handle standard_handle_for_descriptor(i64 fd)
{
    if (fd == 0)
        return GetStdHandle(STD_INPUT_HANDLE);
    if (fd == 1)
        return GetStdHandle(STD_OUTPUT_HANDLE);
    if (fd == 2)
        return GetStdHandle(STD_ERROR_HANDLE);
    return (handle)(u64)fd;
}

i64 __sys_write(i64 fd, const void *buffer, usize count)
{
    dword written = 0;
    const dword size = count > 0xffffffffU ? 0xffffffffU : (dword)count;
    if (!WriteFile(standard_handle_for_descriptor(fd), buffer, size, &written, 0))
        return -1;
    return (i64)written;
}

i64 __sys_read(i64 descriptor, void *buffer, usize count)
{
    dword read = 0;
    const dword size = count > 0xffffffffU ? 0xffffffffU : (dword)count;
    if (!ReadFile(standard_handle_for_descriptor(descriptor), buffer, size, &read, 0))
        return -1;
    return (i64)read;
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
    dword access = 0;
    dword disposition = OPEN_EXISTING;
    switch (mode)
    {
    case 0: access = GENERIC_READ; break;
    case 1: access = GENERIC_WRITE; disposition = CREATE_ALWAYS; break;
    case 2: access = GENERIC_READ | GENERIC_WRITE; break;
    case 3: access = GENERIC_WRITE; disposition = OPEN_ALWAYS; break;
    default: return -1;
    }
    const handle file = CreateFileA(pathname, access, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, disposition, 0, 0);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    if (mode == 3 && !SetFilePointerEx(file, 0, 0, FILE_END))
    {
        CloseHandle(file);
        return -1;
    }
    return (i64)(u64)file;
}

i64 __sys_close(i64 descriptor)
{
    return CloseHandle((handle)(u64)descriptor) ? 0 : -1;
}

i64 __sys_file_size(i64 descriptor)
{
    i64 size = 0;
    return GetFileSizeEx((handle)(u64)descriptor, &size) ? size : -1;
}

__attribute__((noreturn)) void __sys_exit(int status)
{
    ExitProcess((unsigned int)status);
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
    u64 file_time = 0;
    GetSystemTimeAsFileTime(&file_time);
    if (file_time < UNIX_EPOCH_TICKS)
        return -1;
    file_time -= UNIX_EPOCH_TICKS;
    *seconds = (i64)(file_time / WINDOWS_TICKS_PER_SECOND);
    *nanoseconds = (i64)((file_time % WINDOWS_TICKS_PER_SECOND) * 100);
    return 0;
}

void mainCRTStartup(void)
{
    __sys_exit((int)__shaft_entry(0, 0));
}

// Networking — Winsock2

typedef unsigned short uint16;
typedef unsigned int uint32;

#define AF_UNSPEC       0
#define AF_INET         2
#define AF_INET6        23

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define SD_RECEIVE      0
#define SD_SEND         1
#define SD_BOTH         2

#define INVALID_SOCKET  ((handle)(u64)-1)

typedef struct sockaddr
{
    uint16 sa_family;
    char sa_data[14];
} sockaddr;

typedef struct in_addr
{
    uint32 s_addr;
} in_addr;

typedef struct sockaddr_in
{
    uint16 sin_family;
    uint16 sin_port;
    in_addr sin_addr;
    unsigned char sin_zero[8];
} sockaddr_in;

__declspec(dllimport) int __stdcall WSAStartup(
    uint16 version,
    void *data);

__declspec(dllimport) int __stdcall WSACleanup(void);

__declspec(dllimport) handle __stdcall socket(
    int af,
    int type,
    int protocol);

__declspec(dllimport) int __stdcall connect(
    handle socket,
    const sockaddr *name,
    int namelen);

__declspec(dllimport) int __stdcall bind(
    handle socket,
    const sockaddr *name,
    int namelen);

__declspec(dllimport) int __stdcall listen(
    handle socket,
    int backlog);

__declspec(dllimport) handle __stdcall accept(
    handle socket,
    sockaddr *address,
    int *address_length);

__declspec(dllimport) int __stdcall send(
    handle socket,
    const char *buffer,
    int length,
    int flags);

__declspec(dllimport) int __stdcall recv(
    handle socket,
    char *buffer,
    int length,
    int flags);

__declspec(dllimport) int __stdcall sendto(
    handle socket,
    const char *buffer,
    int length,
    int flags,
    const sockaddr *destination,
    int destination_length);

__declspec(dllimport) int __stdcall recvfrom(
    handle socket,
    char *buffer,
    int length,
    int flags,
    sockaddr *source,
    int *source_length);

__declspec(dllimport) int __stdcall shutdown(
    handle socket,
    int how);

__declspec(dllimport) int __stdcall closesocket(handle socket);

static bool32 shaft_network_initialized;

int __sys_network_init(void)
{
    unsigned char data[512];

    if (shaft_network_initialized)
        return 0;

    memset(data, 0, sizeof(data));

    /*
     * MAKEWORD(2, 2)
     */
    const uint16 version = 0x0202;

    const int result = WSAStartup(version, data);

    if (result != 0)
        return -1;

    shaft_network_initialized = 1;
    return 0;
}

void __sys_network_cleanup(void)
{
    if (!shaft_network_initialized)
        return;

    WSACleanup();
    shaft_network_initialized = 0;
}

i64 __sys_socket(int domain, int type, int protocol)
{
    if (__sys_network_init() != 0)
        return -1;

    const handle result = socket(domain, type, protocol);

    if (result == INVALID_SOCKET)
        return -1;

    return (i64)(u64)result;
}

i64 __sys_connect(
    i64 descriptor,
    const sockaddr *address,
    int address_length)
{
    return connect(
        (handle)(u64)descriptor,
        address,
        address_length
    ) == 0 ? 0 : -1;
}

i64 __sys_bind(
    i64 descriptor,
    const sockaddr *address,
    int address_length)
{
    return bind(
        (handle)(u64)descriptor,
        address,
        address_length
    ) == 0 ? 0 : -1;
}

i64 __sys_listen(i64 descriptor, int backlog)
{
    return listen(
        (handle)(u64)descriptor,
        backlog
    ) == 0 ? 0 : -1;
}

i64 __sys_accept(
    i64 descriptor,
    sockaddr *address,
    int *address_length)
{
    const handle result = accept(
        (handle)(u64)descriptor,
        address,
        address_length
    );

    if (result == INVALID_SOCKET)
        return -1;

    return (i64)(u64)result;
}

i64 __sys_send(
    i64 descriptor,
    const void *buffer,
    usize length,
    int flags)
{
    const int size =
        length > 0x7fffffffU ? 0x7fffffff : (int)length;

    return (i64)send(
        (handle)(u64)descriptor,
        (const char *)buffer,
        size,
        flags
    );
}

i64 __sys_recv(
    i64 descriptor,
    void *buffer,
    usize length,
    int flags)
{
    const int size =
        length > 0x7fffffffU ? 0x7fffffff : (int)length;

    return (i64)recv(
        (handle)(u64)descriptor,
        (char *)buffer,
        size,
        flags
    );
}

i64 __sys_sendto(
    i64 descriptor,
    const void *buffer,
    usize length,
    int flags,
    const sockaddr *destination,
    int destination_length)
{
    const int size =
        length > 0x7fffffffU ? 0x7fffffff : (int)length;

    return (i64)sendto(
        (handle)(u64)descriptor,
        (const char *)buffer,
        size,
        flags,
        destination,
        destination_length
    );
}

i64 __sys_recvfrom(
    i64 descriptor,
    void *buffer,
    usize length,
    int flags,
    sockaddr *source,
    int *source_length)
{
    const int size =
        length > 0x7fffffffU ? 0x7fffffff : (int)length;

    return (i64)recvfrom(
        (handle)(u64)descriptor,
        (char *)buffer,
        size,
        flags,
        source,
        source_length
    );
}

i64 __sys_shutdown(i64 descriptor, int how)
{
    return shutdown(
        (handle)(u64)descriptor,
        how
    ) == 0 ? 0 : -1;
}

i64 __sys_socket_close(i64 descriptor)
{
    return closesocket(
        (handle)(u64)descriptor
    ) == 0 ? 0 : -1;
}

static uint16 __net_htons(uint16 value)
{
    return (uint16)(
        ((value & 0x00ffU) << 8) |
        ((value & 0xff00U) >> 8)
    );
}

static uint16 __net_ntohs(uint16 value)
{
    return __net_htons(value);
}

static uint32 __net_htonl(uint32 value)
{
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8)  |
           ((value & 0x00ff0000U) >> 8)  |
           ((value & 0xff000000U) >> 24);
}

static uint32 __net_ntohl(uint32 value)
{
    return __net_htonl(value);
}

i64 __sys_tcp_socket(void)
{
    return __sys_socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );
}

i64 __sys_udp_socket(void)
{
    return __sys_socket(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP
    );
}

i64 __sys_tcp_connect_ipv4(
    i64 descriptor,
    uint32 ipv4,
    uint16 port)
{
    sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);

    return __sys_connect(
        descriptor,
        (const sockaddr *)&address,
        (int)sizeof(address)
    );
}

#define FIONBIO 0x8004667eU
#define WSAEWOULDBLOCK 10035
#define SHAFT_WOULD_BLOCK (-2)
__declspec(dllimport) int __stdcall ioctlsocket(handle socket, dword command, dword *value);
__declspec(dllimport) int __stdcall WSAGetLastError(void);

i64 __sys_tcp_listen_ipv4(uint32 ipv4, uint16 port, int backlog)
{
    i64 descriptor = __sys_tcp_socket();
    if (descriptor < 0)
        return descriptor;
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = __net_htons(port);
    address.sin_addr.s_addr = __net_htonl(ipv4);
    if (__sys_bind(descriptor, (const sockaddr *)&address, sizeof(address)) != 0 || __sys_listen(descriptor, backlog) != 0)
    {
        __sys_socket_close(descriptor);
        return -1;
    }
    return descriptor;
}

i64 __sys_socket_accept(i64 descriptor) { return __sys_accept(descriptor, 0, 0); }
i64 __sys_socket_set_nonblocking(i64 descriptor)
{
    dword enabled = 1;
    return ioctlsocket((handle)(u64)descriptor, FIONBIO, &enabled) == 0 ? 0 : -1;
}
static i64 __sys_normalize_would_block(i64 result)
{
    return result == -1 && WSAGetLastError() == WSAEWOULDBLOCK ? SHAFT_WOULD_BLOCK : result;
}
i64 __sys_socket_try_accept(i64 descriptor) { return __sys_normalize_would_block(__sys_socket_accept(descriptor)); }
i64 __sys_socket_try_send(i64 descriptor, const void *buffer, usize length) { return __sys_normalize_would_block(__sys_send(descriptor, buffer, length, 0)); }
i64 __sys_socket_try_recv(i64 descriptor, void *buffer, usize length) { return __sys_normalize_would_block(__sys_recv(descriptor, buffer, length, 0)); }