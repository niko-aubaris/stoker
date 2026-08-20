/* stoker-sign: release-signing tool for STOKER self-updates.
 *
 * Runs on the release machine only, never ships. Uses the vendored
 * TweetNaCl Ed25519 (same code the app verifies with).
 *
 *   stoker-sign keygen                  create ~/.config/stoker-release/signing.{key,pub}
 *   stoker-sign sign <file> [...]       write <file>.sig (128 hex chars) per file
 *   stoker-sign verify <file>           check <file> against <file>.sig + signing.pub
 *
 * The private key NEVER leaves ~/.config/stoker-release/ (0600). The public
 * half is baked into bpos-dash.cpp as UPDATE_PUBKEY_HEX.
 *
 * build: cc -O2 -o tools/stoker-sign tools/stoker-sign.c tweetnacl.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../tweetnacl.h"

/* TweetNaCl leaves the RNG to us; only keygen uses it */
void randombytes(unsigned char *p, unsigned long long n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read /dev/urandom\n");
        exit(1);
    }
    fclose(f);
}

static const char *keydir(void) {
    static char d[512];
    const char *h = getenv("HOME");
    snprintf(d, sizeof d, "%s/.config/stoker-release", h ? h : ".");
    return d;
}

static void hex_write(const char *path, const unsigned char *b, size_t n, int secret) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
    fprintf(f, "\n");
    fclose(f);
    chmod(path, secret ? 0600 : 0644);
}

static int hex_read(const char *path, unsigned char *b, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    for (size_t i = 0; i < n; i++)
        if (fscanf(f, "%2hhx", &b[i]) != 1) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

static unsigned char *slurp(const char *path, unsigned long long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    if (!b || (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n)) {
        fprintf(stderr, "read failed: %s\n", path);
        exit(1);
    }
    fclose(f);
    *len = (unsigned long long)n;
    return b;
}

int main(int argc, char **argv) {
    char kpath[600], ppath[600];
    snprintf(kpath, sizeof kpath, "%s/signing.key", keydir());
    snprintf(ppath, sizeof ppath, "%s/signing.pub", keydir());

    if (argc >= 2 && !strcmp(argv[1], "keygen")) {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
        struct stat st;
        if (stat(kpath, &st) == 0) {
            fprintf(stderr, "refusing: %s already exists (delete it yourself to rotate)\n", kpath);
            return 1;
        }
        char mk[600];
        snprintf(mk, sizeof mk, "mkdir -p '%s' && chmod 700 '%s'", keydir(), keydir());
        if (system(mk) != 0) return 1;
        crypto_sign_keypair(pk, sk);
        hex_write(kpath, sk, sizeof sk, 1);
        hex_write(ppath, pk, sizeof pk, 0);
        printf("private key: %s (0600, keep it here)\n", kpath);
        printf("public key : %s\npubkey hex : ", ppath);
        for (size_t i = 0; i < sizeof pk; i++) printf("%02x", pk[i]);
        printf("\n^ bake this into UPDATE_PUBKEY_HEX in bpos-dash.cpp\n");
        return 0;
    }

    if (argc >= 3 && !strcmp(argv[1], "sign")) {
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        if (!hex_read(kpath, sk, sizeof sk)) {
            fprintf(stderr, "no signing key at %s (run: stoker-sign keygen)\n", kpath);
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            unsigned long long mlen;
            unsigned char *m = slurp(argv[i], &mlen);
            unsigned char *sm = malloc((size_t)mlen + crypto_sign_BYTES);
            unsigned long long smlen;
            crypto_sign(sm, &smlen, m, mlen, sk);
            char spath[1024];
            snprintf(spath, sizeof spath, "%s.sig", argv[i]);
            hex_write(spath, sm, crypto_sign_BYTES, 0);  /* detached: first 64 bytes */
            printf("signed %s -> %s\n", argv[i], spath);
            free(m);
            free(sm);
        }
        return 0;
    }

    if (argc >= 3 && !strcmp(argv[1], "verify")) {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES], sig[crypto_sign_BYTES];
        if (!hex_read(ppath, pk, sizeof pk)) {
            fprintf(stderr, "no public key at %s\n", ppath);
            return 1;
        }
        char spath[1024];
        snprintf(spath, sizeof spath, "%s.sig", argv[2]);
        if (!hex_read(spath, sig, sizeof sig)) {
            fprintf(stderr, "cannot read %s\n", spath);
            return 1;
        }
        unsigned long long mlen;
        unsigned char *m = slurp(argv[2], &mlen);
        unsigned char *sm = malloc((size_t)mlen + crypto_sign_BYTES);
        unsigned char *out = malloc((size_t)mlen + crypto_sign_BYTES);
        memcpy(sm, sig, crypto_sign_BYTES);
        memcpy(sm + crypto_sign_BYTES, m, (size_t)mlen);
        unsigned long long outlen;
        int rc = crypto_sign_open(out, &outlen, sm, mlen + crypto_sign_BYTES, pk);
        printf("%s: %s\n", argv[2], rc == 0 ? "signature OK" : "signature FAILED");
        return rc == 0 ? 0 : 1;
    }

    fprintf(stderr, "usage: stoker-sign keygen | sign <file>... | verify <file>\n");
    return 1;
}
