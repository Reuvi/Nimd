#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_RAW   256
#define MAX_FIELDS 8

typedef struct {
    char raw[MAX_RAW];      // full frame "0|DD|....|"
    int  raw_len;

    int  len;               // payload length (DD)
    char type[5];           // 4 chars + '\0'

    char work[MAX_RAW];     // mutable payload copy
    char *fields[MAX_FIELDS];
    int field_count;
} NgpMsg;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, fmt, ...) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL: " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static ssize_t read_exact(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r == 0) return 0;            // EOF
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char*)buf + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return (ssize_t)sent;
}

static int connect_tcp(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL, *p = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static int count_char(const char *s, char c) {
    int n = 0;
    for (; *s; s++) if (*s == c) n++;
    return n;
}

// Strict recv of server frame: requires header "0|DD|" (two digits!)
static int ngp_recv(int fd, NgpMsg *m) {
    memset(m, 0, sizeof(*m));

    char hdr[5];
    ssize_t r = read_exact(fd, hdr, sizeof(hdr));
    if (r == 0) return 0;     // EOF
    if (r < 0) return -1;

    if (!(hdr[0] == '0' && hdr[1] == '|' && isdigit((unsigned char)hdr[2]) &&
          isdigit((unsigned char)hdr[3]) && hdr[4] == '|')) {
        return -2; // bad frame header
    }

    int len = (hdr[2] - '0') * 10 + (hdr[3] - '0');
    if (len <= 0 || len >= (MAX_RAW - 6)) return -2;

    char payload[MAX_RAW];
    r = read_exact(fd, payload, (size_t)len);
    if (r == 0) return 0;
    if (r < 0) return -1;

    if (payload[len - 1] != '|') return -2;

    // Save raw frame (bar counting happens on THIS, not on the tokenized work buffer)
    memcpy(m->raw, hdr, 5);
    memcpy(m->raw + 5, payload, (size_t)len);
    m->raw_len = 5 + len;
    m->raw[m->raw_len] = '\0';

    m->len = len;

    // Copy payload into mutable work buffer and parse fields INCLUDING empty ones
    memcpy(m->work, payload, (size_t)len);
    m->work[len] = '\0';

    // type must be 4 chars then '|'
    if ((int)strlen(m->work) < 5) return -2;
    if (m->work[4] != '|') return -2;
    memcpy(m->type, m->work, 4);
    m->type[4] = '\0';
    m->work[4] = '\0';

    // Fields start after "TYPE|"
    char *start = m->work + 5;
    int nf = 0;

    // Scan until we hit the '\0' we placed at end. Every '|' becomes '\0' and emits a field.
    for (char *p = start; *p; p++) {
        if (*p == '|') {
            if (nf >= MAX_FIELDS) return -2;
            *p = '\0';
            m->fields[nf++] = start;
            start = p + 1;
        }
    }

    // Special case: payload could be "OVER|...||" where the last field is empty.
    // That empty field is represented by a final '|' which is inside the string before our '\0'.
    // The loop above *will* capture it because the second-to-last character is '|'.
    m->field_count = nf;

    return 1;
}

static int expected_bars_for_type(const char *type) {
    if (strcmp(type, "WAIT") == 0) return 3; // 0|DD|WAIT|
    if (strcmp(type, "NAME") == 0) return 5; // +2 fields
    if (strcmp(type, "PLAY") == 0) return 5; // +2 fields
    if (strcmp(type, "OVER") == 0) return 6; // +3 fields
    if (strcmp(type, "FAIL") == 0) return 4; // +1 field
    return -1;
}

static void send_raw(int fd, const char *s) {
    (void)write_all(fd, s, strlen(s));
}

static void send_payload(int fd, const char *payload) {
    size_t len = strlen(payload);
    char frame[MAX_RAW];
    snprintf(frame, sizeof(frame), "0|%02zu|%s", len, payload);
    send_raw(fd, frame);
}

static void send_open(int fd, const char *name) {
    char payload[MAX_RAW];
    snprintf(payload, sizeof(payload), "OPEN|%s|", name);
    send_payload(fd, payload);
}

static void send_move(int fd, int pile, int qty) {
    char payload[MAX_RAW];
    snprintf(payload, sizeof(payload), "MOVE|%d|%d|", pile, qty);
    send_payload(fd, payload);
}

static void expect_msg(int fd, const char *type, int fields) {
    NgpMsg m;
    int rc = ngp_recv(fd, &m);
    CHECK(rc == 1, "expected %s but recv failed (rc=%d)", type, rc);
    if (rc != 1) return;

    CHECK(strcmp(m.type, type) == 0, "expected type=%s got type=%s (raw=%s)", type, m.type, m.raw);
    CHECK(m.field_count == fields, "%s must have %d field(s), got %d (raw=%s)", type, fields, m.field_count, m.raw);

    int expbars = expected_bars_for_type(m.type);
    if (expbars != -1) {
        int bars = count_char(m.raw, '|');
        CHECK(bars == expbars, "%s must have %d total '|' chars, got %d (raw=%s)", m.type, expbars, bars, m.raw);
    }

    // Spec sanity: length must be exactly 2 digits in header (we already enforced in ngp_recv)
}

static void expect_fail(int fd, const char *expected_prefix) {
    NgpMsg m;
    int rc = ngp_recv(fd, &m);
    CHECK(rc == 1, "expected FAIL but recv failed (rc=%d)", rc);
    if (rc != 1) return;

    CHECK(strcmp(m.type, "FAIL") == 0, "expected FAIL got %s (raw=%s)", m.type, m.raw);
    CHECK(m.field_count == 1, "FAIL must have 1 field (raw=%s)", m.raw);

    if (m.field_count == 1 && expected_prefix) {
        CHECK(strncmp(m.fields[0], expected_prefix, strlen(expected_prefix)) == 0,
              "FAIL field must start with '%s' got '%s' (raw=%s)", expected_prefix, m.fields[0], m.raw);
    }

    int bars = count_char(m.raw, '|');
    CHECK(bars == 4, "FAIL must have 4 total '|' chars, got %d (raw=%s)", bars, m.raw);
}

static void expect_close(int fd) {
    NgpMsg m;
    int rc = ngp_recv(fd, &m);
    CHECK(rc == 0, "expected server to close, but got message (raw=%s)", m.raw);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    const char *port = argv[2];

    printf("NGP Spec Tester -> host=%s port=%s\n\n", host, port);

    // [TEST] bad frame: one-digit length
    {
        printf("[TEST] bad frame: one-digit length (should FAIL 10 Invalid and close)\n");
        int fd = connect_tcp(host, port);
        CHECK(fd >= 0, "connect failed");
        if (fd >= 0) {
            send_raw(fd, "0|9|OPEN|R|"); // intentionally invalid framing
            expect_fail(fd, "10");
            expect_close(fd);
            close(fd);
        }
        printf("\n");
    }

    // [TEST] bad OPEN: extra '|' (OPEN|L||)
    {
        printf("[TEST] bad OPEN: extra '|' (OPEN|L||) should FAIL 10 Invalid and close\n");
        int fd = connect_tcp(host, port);
        CHECK(fd >= 0, "connect failed");
        if (fd >= 0) {
            send_raw(fd, "0|08|OPEN|L||"); // invalid: empty extra field
            expect_fail(fd, "10");
            expect_close(fd);
            close(fd);
        }
        printf("\n");
    }

    // [TEST] OPEN long name (>72)
    {
        printf("[TEST] OPEN long name (>72) should FAIL 21 Long Name and close\n");
        int fd = connect_tcp(host, port);
        CHECK(fd >= 0, "connect failed");
        if (fd >= 0) {
            char name[90];
            memset(name, 'A', 73);
            name[73] = '\0';
            send_open(fd, name);
            expect_fail(fd, "21");
            expect_close(fd);
            close(fd);
        }
        printf("\n");
    }

    // [TEST] OPEN twice
    {
        printf("[TEST] OPEN twice should FAIL 23 Already Open and close\n");
        int fd = connect_tcp(host, port);
        CHECK(fd >= 0, "connect failed");
        if (fd >= 0) {
            send_open(fd, "Once");
            expect_msg(fd, "WAIT", 0);

            send_open(fd, "Twice");
            expect_fail(fd, "23");
            expect_close(fd);
            close(fd);
        }
        printf("\n");
    }

    // [TEST] MOVE before NAME
    {
        printf("[TEST] MOVE before NAME should FAIL 24 Not Playing and close\n");
        int fd = connect_tcp(host, port);
        CHECK(fd >= 0, "connect failed");
        if (fd >= 0) {
            send_open(fd, "Solo");
            expect_msg(fd, "WAIT", 0);

            send_move(fd, 1, 1);
            expect_fail(fd, "24");
            expect_close(fd);
            close(fd);
        }
        printf("\n");
    }

    // [TEST] name already in use
    {
        printf("[TEST] name already in use should FAIL 22 Already Playing and close\n");
        int fd1 = connect_tcp(host, port);
        CHECK(fd1 >= 0, "connect fd1 failed");
        if (fd1 >= 0) {
            send_open(fd1, "DupName");
            expect_msg(fd1, "WAIT", 0);
        }

        int fd2 = connect_tcp(host, port);
        CHECK(fd2 >= 0, "connect fd2 failed");
        if (fd2 >= 0) {
            send_open(fd2, "DupName");
            expect_fail(fd2, "22");
            expect_close(fd2);
            close(fd2);
        }

        if (fd1 >= 0) close(fd1);
        printf("\n");
    }

    // [TEST] full match + FAIL 31/32/33 + normal OVER
    {
        printf("[TEST] full match: NAME, PLAY, FAIL 31/32/33, normal OVER\n");

        int p1 = connect_tcp(host, port);
        int p2 = connect_tcp(host, port);
        CHECK(p1 >= 0 && p2 >= 0, "connect failed p1=%d p2=%d", p1, p2);

        if (p1 >= 0 && p2 >= 0) {
            send_open(p1, "AliceT");
            expect_msg(p1, "WAIT", 0);

            send_open(p2, "BobT");
            expect_msg(p2, "WAIT", 0);

            // Both should get NAME then PLAY
            expect_msg(p1, "NAME", 2);
            expect_msg(p2, "NAME", 2);

            expect_msg(p1, "PLAY", 2);
            expect_msg(p2, "PLAY", 2);

            // Impatient: P2 moves during P1's turn
            send_move(p2, 1, 1);
            expect_fail(p2, "31"); // should NOT close

            // P1 invalid pile index
            send_move(p1, 6, 1);
            expect_fail(p1, "32");

            // P1 invalid qty
            send_move(p1, 1, 9);
            expect_fail(p1, "33");

            // Now play a fast full game: P1 wins
            send_move(p1, 1, 1);
            expect_msg(p1, "PLAY", 2);
            expect_msg(p2, "PLAY", 2);

            send_move(p2, 2, 3);
            expect_msg(p1, "PLAY", 2);
            expect_msg(p2, "PLAY", 2);

            send_move(p1, 3, 5);
            expect_msg(p1, "PLAY", 2);
            expect_msg(p2, "PLAY", 2);

            send_move(p2, 4, 7);
            expect_msg(p1, "PLAY", 2);
            expect_msg(p2, "PLAY", 2);

            send_move(p1, 5, 9);

            // Both should receive OVER then close
            expect_msg(p1, "OVER", 3);
            expect_msg(p2, "OVER", 3);

            expect_close(p1);
            expect_close(p2);

            close(p1);
            close(p2);
        }
        printf("\n");
    }

    // [TEST] forfeit: disconnect during game => remaining gets OVER ...|Forfeit|
    {
        printf("[TEST] forfeit: disconnect during game => remaining gets OVER ...|Forfeit|\n");

        int a = connect_tcp(host, port);
        int b = connect_tcp(host, port);
        CHECK(a >= 0 && b >= 0, "connect failed a=%d b=%d", a, b);

        if (a >= 0 && b >= 0) {
            send_open(a, "ForfA");
            expect_msg(a, "WAIT", 0);

            send_open(b, "ForfB");
            expect_msg(b, "WAIT", 0);

            expect_msg(a, "NAME", 2);
            expect_msg(b, "NAME", 2);

            expect_msg(a, "PLAY", 2);
            expect_msg(b, "PLAY", 2);

            // Forfeit: kill B mid-game
            close(b);

            NgpMsg m;
            int rc = ngp_recv(a, &m);
            CHECK(rc == 1, "expected OVER after forfeit but recv failed rc=%d", rc);
            if (rc == 1) {
                CHECK(strcmp(m.type, "OVER") == 0, "expected OVER got %s (raw=%s)", m.type, m.raw);
                CHECK(m.field_count == 3, "OVER must have 3 fields (raw=%s)", m.raw);
                if (m.field_count == 3) {
                    CHECK(strcmp(m.fields[2], "Forfeit") == 0, "OVER third field must be Forfeit, got '%s' (raw=%s)", m.fields[2], m.raw);
                }
            }

            expect_close(a);
            close(a);
        }

        printf("\n");
    }

    printf("PASS=%d  FAIL=%d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}