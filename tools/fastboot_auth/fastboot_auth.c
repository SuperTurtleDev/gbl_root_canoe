/*
 * fastboot_auth - pair this computer with an SFB BDS and unlock its fastboot.
 *
 *   fastboot_auth pair <ip:port>
 *   fastboot_auth auth [-d <device>] [-f <fastboot>]
 *
 * pair: connects to the phone's pairing service (the EFI Boot Mgr app shows
 * the address and a six-digit code), proves the pairing code, hands over a
 * self-signed certificate CN=user@host and answers a challenge signed with
 * its private key. The phone installs the certificate into
 * AUTH_CERTS of the config container; the BDS reads that directory at boot.
 *
 * auth: the SSH-style unlock of the running BDS, using the key stored by a
 * previous pair: fetch sb_challenge, sign SHA-256(nonce) with RSA PKCS#1 v1.5
 * and flash sb_challenge.sig. -d passes a device serial to the fastboot
 * commands (-s), -f selects the fastboot binary (default: fastboot in PATH).
 *
 * Key material lives in $HOME/.fastboot_auth (key.pem, cert.pem). The private
 * key never leaves this directory.
 *
 * Build: make -C tools/fastboot_auth   (needs OpenSSL headers and libcrypto)
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#define PAIR_LINE_MAX   65536
#define PAIR_NONCE_MAX  64

static void die(const char *msg)
{
    fprintf(stderr, "fastboot_auth: %s\n", msg);
    exit(1);
}

/* ---- small helpers ------------------------------------------------------ */

static char *b64_encode(const unsigned char *in, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);

    if (out == NULL) {
        return NULL;
    }
    EVP_EncodeBlock((unsigned char *)out, in, (int)len);
    out[out_len] = '\0';
    return out;
}

static unsigned char *b64_decode(const char *in, size_t *out_len)
{
    size_t n = strlen(in);
    unsigned char *out;
    int len;

    if (n == 0 || (n % 4) != 0) {
        return NULL;
    }
    out = malloc(n / 4 * 3 + 1);
    if (out == NULL) {
        return NULL;
    }
    len = EVP_DecodeBlock(out, (const unsigned char *)in, (int)n);
    if (len < 0) {
        free(out);
        return NULL;
    }
    if (in[n - 1] == '=') {
        len--;
    }
    if (n >= 2 && in[n - 2] == '=') {
        len--;
    }
    *out_len = (size_t)len;
    return out;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_line(int fd, const char *prefix, const char *payload)
{
    char *line;
    size_t len;
    int rc;

    if (payload == NULL) {
        len = strlen(prefix) + 2;
        line = malloc(len);
        if (line == NULL) {
            return -1;
        }
        snprintf(line, len, "%s\n", prefix);
    } else {
        len = strlen(prefix) + strlen(payload) + 3;
        line = malloc(len);
        if (line == NULL) {
            return -1;
        }
        snprintf(line, len, "%s %s\n", prefix, payload);
    }
    rc = write_all(fd, line, strlen(line));
    free(line);
    return rc;
}

/* One LF-terminated line, CR stripped, NUL terminated. NULL on EOF/overflow. */
static char *read_line(int fd, char *buf, size_t cap)
{
    size_t at = 0;

    while (at + 1 < cap) {
        char ch;
        ssize_t n = read(fd, &ch, 1);

        if (n <= 0) {
            return NULL;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            buf[at++] = ch;
        }
    }
    buf[at] = '\0';
    return buf;
}

/* ---- key material ------------------------------------------------------- */

static char *config_dir(void)
{
    const char *home = getenv("HOME");
    char *dir;

    if (home == NULL || home[0] == '\0') {
        return NULL;
    }
    dir = malloc(strlen(home) + 32);
    if (dir != NULL) {
        sprintf(dir, "%s/.fastboot_auth", home);
    }
    return dir;
}

static void ensure_dir(const char *dir)
{
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        die("cannot create ~/.fastboot_auth");
    }
}

static char *make_cn(void)
{
    const char *user = getenv("USER");
    char host[256];
    char *cn;

    if (user == NULL || user[0] == '\0') {
        user = "user";
    }
    if (gethostname(host, sizeof(host) - 1) != 0) {
        strcpy(host, "host");
    }
    host[sizeof(host) - 1] = '\0';
    cn = malloc(strlen(user) + strlen(host) + 2);
    if (cn != NULL) {
        sprintf(cn, "%s@%s", user, host);
    }
    return cn;
}

static EVP_PKEY *load_key(const char *dir)
{
    char path[4096];
    FILE *f;
    EVP_PKEY *key;

    snprintf(path, sizeof(path), "%s/key.pem", dir);
    f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return key;
}

static X509 *load_cert(const char *dir)
{
    char path[4096];
    FILE *f;
    X509 *cert;

    snprintf(path, sizeof(path), "%s/cert.pem", dir);
    f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    return cert;
}

static void save_key(EVP_PKEY *key, const char *dir)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/key.pem", dir);
    f = fopen(path, "w");
    if (f == NULL) {
        die("cannot write ~/.fastboot_auth/key.pem");
    }
    chmod(path, 0600);
    PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL);
    fclose(f);
}

static void save_cert(X509 *cert, const char *dir)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/cert.pem", dir);
    f = fopen(path, "w");
    if (f == NULL) {
        die("cannot write ~/.fastboot_auth/cert.pem");
    }
    chmod(path, 0644);
    PEM_write_X509(f, cert);
    fclose(f);
}

static X509 *make_cert(EVP_PKEY *key, const char *cn)
{
    X509 *cert = X509_new();
    X509_NAME *name;
    unsigned char serial[8];

    if (cert == NULL) {
        die("out of memory");
    }
    if (RAND_bytes(serial, sizeof(serial)) != 1) {
        die("no random source");
    }
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert),
                     (long)(serial[0] | (serial[1] << 8) | (serial[2] << 16) |
                            ((long)serial[3] << 24)) & 0x7fffffffL);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 3650);
    X509_set_pubkey(cert, key);

    name = X509_get_subject_name(cert);
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                   (const unsigned char *)cn, -1, -1, 0) != 1) {
        die("cannot set certificate CN");
    }
    X509_set_issuer_name(cert, name);
    if (X509_sign(cert, key, EVP_sha256()) <= 0) {
        die("cannot self-sign the certificate");
    }
    return cert;
}

static unsigned char *sign_sha256(EVP_PKEY *key, const unsigned char *msg,
                                  size_t len, size_t *sig_len)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char *sig;
    size_t n = 0;

    if (ctx == NULL ||
        EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) != 1 ||
        EVP_DigestSign(ctx, NULL, &n, msg, len) != 1) {
        EVP_MD_CTX_free(ctx);
        die("signing failed");
    }
    sig = malloc(n);
    if (sig == NULL || EVP_DigestSign(ctx, sig, &n, msg, len) != 1) {
        EVP_MD_CTX_free(ctx);
        die("signing failed");
    }
    EVP_MD_CTX_free(ctx);
    *sig_len = n;
    return sig;
}

/* ---- pairing ------------------------------------------------------------ */

static int connect_to(const char *target)
{
    char host[256];
    const char *colon = strrchr(target, ':');
    const char *port;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int fd = -1;
    size_t host_len;

    if (colon == NULL || colon == target) {
        die("pairing target must be <ip:port>");
    }
    host_len = (size_t)(colon - target);
    if (host_len >= sizeof(host)) {
        die("host name too long");
    }
    memcpy(host, target, host_len);
    host[host_len] = '\0';
    port = colon + 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        die("cannot resolve the pairing address");
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        die("cannot connect to the pairing service");
    }
    return fd;
}

static void prompt_code(char *out, size_t cap)
{
    struct termios old;
    struct termios noecho;
    int quiet = isatty(STDIN_FILENO);
    size_t at = 0;

    printf("Pairing code: ");
    fflush(stdout);
    if (quiet && tcgetattr(STDIN_FILENO, &old) == 0) {
        noecho = old;
        noecho.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    }
    while (at + 1 < cap) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) <= 0 || ch == '\n') {
            break;
        }
        if (ch != '\r') {
            out[at++] = ch;
        }
    }
    out[at] = '\0';
    if (quiet && tcgetattr(STDIN_FILENO, &old) == 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
    }
    printf("\n");
}

static int cmd_pair(const char *target)
{
    char *dir = config_dir();
    EVP_PKEY *key;
    X509 *cert;
    char *cn;
    unsigned char *der = NULL;
    int der_len;
    int fd;
    char line[PAIR_LINE_MAX];
    char code[64];
    char *b64;
    unsigned char *nonce = NULL;
    size_t nonce_len = 0;
    unsigned char *sig = NULL;
    size_t sig_len = 0;
    int ok = 0;

    if (dir == NULL) {
        die("HOME is not set");
    }
    ensure_dir(dir);

    /* Reuse the identity across pairings: the phone keys a computer by its
     * CN, so re-pairing replaces the previous certificate. */
    key = load_key(dir);
    cert = load_cert(dir);
    if (key == NULL || cert == NULL) {
        if (key != NULL) {
            EVP_PKEY_free(key);
        }
        if (cert != NULL) {
            X509_free(cert);
        }
        cn = make_cn();
        if (cn == NULL) {
            die("out of memory");
        }
        key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048);
        if (key == NULL) {
            die("cannot generate an RSA-2048 key");
        }
        cert = make_cert(key, cn);
        printf("Generated %s\n", cn);
        free(cn);
    }

    der_len = i2d_X509(cert, &der);
    if (der_len <= 0) {
        die("cannot encode the certificate");
    }
    b64 = b64_encode(der, (size_t)der_len);
    if (b64 == NULL) {
        die("out of memory");
    }

    fd = connect_to(target);
    prompt_code(code, sizeof(code));

    if (send_line(fd, "PAIR", code) != 0 ||
        read_line(fd, line, sizeof(line)) == NULL) {
        die("the pairing service did not answer");
    }
    if (strcmp(line, "OK") != 0) {
        fprintf(stderr, "fastboot_auth: pairing rejected: %s\n", line);
        goto out;
    }

    if (send_line(fd, "CERT", b64) != 0 ||
        read_line(fd, line, sizeof(line)) == NULL) {
        die("the pairing service did not answer");
    }
    if (strncmp(line, "CHALLENGE ", 10) != 0) {
        fprintf(stderr, "fastboot_auth: pairing rejected: %s\n", line);
        goto out;
    }
    nonce = b64_decode(line + 10, &nonce_len);
    if (nonce == NULL || nonce_len == 0 || nonce_len > PAIR_NONCE_MAX) {
        fprintf(stderr, "fastboot_auth: bad challenge\n");
        goto out;
    }

    sig = sign_sha256(key, nonce, nonce_len, &sig_len);
    b64 = b64_encode(sig, sig_len);
    if (b64 == NULL) {
        die("out of memory");
    }
    if (send_line(fd, "SIG", b64) != 0 ||
        read_line(fd, line, sizeof(line)) == NULL) {
        die("the pairing service did not answer");
    }
    if (strncmp(line, "DONE", 4) != 0) {
        fprintf(stderr, "fastboot_auth: pairing failed: %s\n", line);
        goto out;
    }

    /* The phone accepted the certificate; keep this identity for `auth`. */
    save_key(key, dir);
    save_cert(cert, dir);
    printf("Paired. Certificate stored in %s\n", dir);
    printf("Reboot the phone once so the BDS loads AUTH_CERTS, "
           "then run: fastboot_auth auth\n");
    ok = 1;

out:
    close(fd);
    free(der);
    free(nonce);
    free(sig);
    X509_free(cert);
    EVP_PKEY_free(key);
    free(dir);
    return ok ? 0 : 1;
}

/* ---- unlock ------------------------------------------------------------- */

static int run_fastboot(const char *fb, const char *serial,
                        const char *const *args, int nargs)
{
    char **argv = calloc((size_t)nargs + 4, sizeof(char *));
    pid_t pid;
    int at = 0;
    int status;
    int i;

    if (argv == NULL) {
        die("out of memory");
    }
    argv[at++] = (char *)fb;
    if (serial != NULL) {
        argv[at++] = "-s";
        argv[at++] = (char *)serial;
    }
    for (i = 0; i < nargs; i++) {
        argv[at++] = (char *)args[i];
    }
    argv[at] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(fb, argv);
        fprintf(stderr, "fastboot_auth: cannot run %s: %s\n", fb,
                strerror(errno));
        _exit(127);
    }
    free(argv);
    if (pid < 0 || waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *buf;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

static int write_file(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (f == NULL) {
        return -1;
    }
    ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}

static int cmd_auth(const char *serial, const char *fb)
{
    char *dir = config_dir();
    EVP_PKEY *key;
    char tmp[] = "/tmp/fastboot_auth.XXXXXX";
    char challenge[4096];
    char sig_path[4096];
    unsigned char *nonce = NULL;
    unsigned char *sig = NULL;
    size_t nonce_len = 0;
    size_t sig_len = 0;
    const char *fetch_args[4];
    const char *flash_args[4];
    int rc = 1;

    if (dir == NULL) {
        die("HOME is not set");
    }
    key = load_key(dir);
    if (key == NULL) {
        die("no paired key - run `fastboot_auth pair <ip:port>` first");
    }
    if (mkdtemp(tmp) == NULL) {
        die("cannot create a temporary directory");
    }
    snprintf(challenge, sizeof(challenge), "%s/challenge.bin", tmp);
    snprintf(sig_path, sizeof(sig_path), "%s/challenge.sig", tmp);

    fetch_args[0] = "fetch";
    fetch_args[1] = "sb_challenge";
    fetch_args[2] = challenge;
    fetch_args[3] = NULL;
    if (run_fastboot(fb, serial, fetch_args, 3) != 0) {
        fprintf(stderr, "fastboot_auth: fastboot fetch failed\n");
        goto out;
    }
    nonce = read_file(challenge, &nonce_len);
    if (nonce == NULL) {
        fprintf(stderr, "fastboot_auth: no challenge received\n");
        goto out;
    }
    sig = sign_sha256(key, nonce, nonce_len, &sig_len);
    if (write_file(sig_path, sig, sig_len) != 0) {
        fprintf(stderr, "fastboot_auth: cannot write the signature\n");
        goto out;
    }
    flash_args[0] = "flash";
    flash_args[1] = "sb_challenge.sig";
    flash_args[2] = sig_path;
    flash_args[3] = NULL;
    if (run_fastboot(fb, serial, flash_args, 3) != 0) {
        fprintf(stderr, "fastboot_auth: fastboot flash failed\n");
        goto out;
    }
    printf("Unlocked for this boot.\n");
    rc = 0;

out:
    unlink(challenge);
    unlink(sig_path);
    rmdir(tmp);
    free(nonce);
    free(sig);
    EVP_PKEY_free(key);
    free(dir);
    return rc;
}

/* ---- main --------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage: fastboot_auth pair <ip:port>\n"
            "       fastboot_auth auth [-d <device>] [-f <fastboot>]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
    }
    if (strcmp(argv[1], "pair") == 0) {
        if (argc != 3) {
            usage();
        }
        return cmd_pair(argv[2]);
    }
    if (strcmp(argv[1], "auth") == 0) {
        const char *serial = NULL;
        const char *fb = "fastboot";
        int i;

        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                serial = argv[++i];
            } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                fb = argv[++i];
            } else {
                usage();
            }
        }
        return cmd_auth(serial, fb);
    }
    usage();
    return 2;
}
