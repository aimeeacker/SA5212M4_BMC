#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#define UIMAGE_MAGIC        0x27051956
#define IH_OS_LINUX         5
#define IH_ARCH_ARM         2
#define IH_TYPE_RAMDISK     3
#define IH_COMP_NONE        0
#define UIMAGE_HDR_SIZE     64
#define TARGET_CRAMFS_SIZE  16703488  /* 0xFEE000 */
#define IROOT_TOTAL_SIZE    (UIMAGE_HDR_SIZE + TARGET_CRAMFS_SIZE) /* 0xFEE040 = 16,703,552 */
#define IROOT_ROM_OFFSET    0x150000

static const uint32_t TEA_KEY[4] = {
    0x64756162, 0x65746172, 0x3531313d, 0x00303032  /* \"baudrate=115200\\0\" */
};

static inline uint32_t bswap32(uint32_t x) {
    return ((x << 24) & 0xff000000) |
           ((x <<  8) & 0x00ff0000) |
           ((x >>  8) & 0x0000ff00) |
           ((x >> 24) & 0x000000ff);
}

static void tea_encrypt_block(uint32_t *v, const uint32_t *k) {
    uint32_t v0 = v[0], v1 = v[1], sum = 0;
    uint32_t delta = 0x9e3779b9;
    for (int i = 0; i < 32; i++) {
        sum += delta;
        v0 += ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        v1 += ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
    }
    v[0] = v0;
    v[1] = v1;
}

static void tea_decrypt_block(uint32_t *v, const uint32_t *k) {
    uint32_t v0 = v[0], v1 = v[1];
    uint32_t sum = 0xC6EF3720;
    uint32_t delta = 0x9E3779B9;
    for (int i = 0; i < 32; i++) {
        v1 -= (((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]));
        v0 -= (((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]));
        sum -= delta;
    }
    v[0] = v0;
    v[1] = v1;
}

static int do_encrypt(const char *in_cramfs, const char *out_enc) {
    FILE *fin = fopen(in_cramfs, "rb");
    if (!fin) {
        perror("[-] fopen in_cramfs");
        return 1;
    }

    fseek(fin, 0, SEEK_END);
    long file_sz = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    if (file_sz > TARGET_CRAMFS_SIZE) {
        fprintf(stderr, "[-] Error: cramfs image size %ld exceeds maximum partition size %d!\\n",
                file_sz, TARGET_CRAMFS_SIZE);
        fclose(fin);
        return 1;
    }

    size_t cramfs_sz = TARGET_CRAMFS_SIZE;
    size_t total_sz = IROOT_TOTAL_SIZE;

    uint8_t *buf = (uint8_t *)calloc(1, total_sz);
    if (!buf) {
        perror("[-] calloc");
        fclose(fin);
        return 1;
    }

    if (fread(buf + UIMAGE_HDR_SIZE, 1, file_sz, fin) != (size_t)file_sz) {
        perror("[-] fread cramfs");
        free(buf);
        fclose(fin);
        return 1;
    }
    fclose(fin);

    if (file_sz < TARGET_CRAMFS_SIZE) {
        printf("[*] Padding cramfs from %ld to %zu bytes with zeros.\\n", file_sz, cramfs_sz);
    }

    /* 1. Calculate CRC32 of CramFS payload */
    uint32_t dcrc = crc32(0L, Z_NULL, 0);
    dcrc = crc32(dcrc, buf + UIMAGE_HDR_SIZE, cramfs_sz);
    printf("[+] CramFS CRC32 (dcrc): 0x%08X\\n", dcrc);

    /* 2. Build 64-byte uImage header */
    uint8_t *hdr = buf;
    uint32_t magic = bswap32(UIMAGE_MAGIC);
    uint32_t hcrc = 0;
    uint32_t tm = bswap32((uint32_t)time(NULL));
    uint32_t sz_be = bswap32((uint32_t)cramfs_sz);
    uint32_t load = 0;
    uint32_t ep = 0;
    uint32_t dcrc_be = bswap32(dcrc);

    memcpy(hdr + 0,  &magic, 4);
    memcpy(hdr + 4,  &hcrc, 4);
    memcpy(hdr + 8,  &tm, 4);
    memcpy(hdr + 12, &sz_be, 4);
    memcpy(hdr + 16, &load, 4);
    memcpy(hdr + 20, &ep, 4);
    memcpy(hdr + 24, &dcrc_be, 4);
    hdr[28] = IH_OS_LINUX;
    hdr[29] = IH_ARCH_ARM;
    hdr[30] = IH_TYPE_RAMDISK;
    hdr[31] = IH_COMP_NONE;
    memset(hdr + 32, 0, 32); /* Image name */

    /* 3. Calculate Header CRC32 (with hcrc=0) */
    hcrc = crc32(0L, Z_NULL, 0);
    hcrc = crc32(hcrc, hdr, UIMAGE_HDR_SIZE);
    uint32_t hcrc_be = bswap32(hcrc);
    memcpy(hdr + 4, &hcrc_be, 4);
    printf("[+] uImage Header CRC32 (hcrc): 0x%08X\\n", hcrc);

    /* 4. TEA encrypt 64-byte header + cramfs payload */
    for (size_t i = 0; i < total_sz; i += 8) {
        tea_encrypt_block((uint32_t *)(buf + i), TEA_KEY);
    }
    printf("[+] Successfully encrypted %zu bytes (0x%zX) using TEA!\\n", total_sz, total_sz);

    /* 5. Write output file */
    FILE *fout = fopen(out_enc, "wb");
    if (!fout) {
        perror("[-] fopen out_enc");
        free(buf);
        return 1;
    }
    if (fwrite(buf, 1, total_sz, fout) != total_sz) {
        perror("[-] fwrite out_enc");
        fclose(fout);
        free(buf);
        return 1;
    }
    fclose(fout);
    free(buf);

    printf("[✓] Encrypted iroot payload saved to: %s (%zu bytes)\\n", out_enc, total_sz);
    return 0;
}

static int do_decrypt(const char *in_path, const char *out_path) {
    FILE *fin = fopen(in_path, "rb");
    if (!fin) {
        perror("[-] fopen in");
        return 1;
    }

    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    size_t sz = IROOT_TOTAL_SIZE;
    if (fsize >= IROOT_ROM_OFFSET + (long)sz) {
        /* Full 32MB ROM */
        fseek(fin, IROOT_ROM_OFFSET, SEEK_SET);
    } else if (fsize >= (long)sz) {
        /* Raw iroot payload */
        fseek(fin, 0, SEEK_SET);
    } else {
        fprintf(stderr, "[-] Error: Input file size (%ld bytes) is too small to contain iroot payload!\\n", fsize);
        fclose(fin);
        return 1;
    }

    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        perror("[-] malloc");
        fclose(fin);
        return 1;
    }
    if (fread(buf, 1, sz, fin) != sz) {
        perror("[-] fread");
        free(buf);
        fclose(fin);
        return 1;
    }
    fclose(fin);

    for (size_t i = 0; i < sz; i += 8) {
        tea_decrypt_block((uint32_t *)(buf + i), TEA_KEY);
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        perror("[-] fopen out");
        free(buf);
        return 1;
    }

    /* Skip 64-byte uImage header to output pure CramFS */
    size_t cramfs_sz = sz - UIMAGE_HDR_SIZE;
    if (fwrite(buf + UIMAGE_HDR_SIZE, 1, cramfs_sz, fout) != cramfs_sz) {
        perror("[-] fwrite");
        fclose(fout);
        free(buf);
        return 1;
    }
    fclose(fout);
    free(buf);

    printf("[✓] Successfully decrypted and saved %s (%zu bytes)!\\n", out_path, cramfs_sz);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *prog = argv[0];
    const char *mode = NULL;
    const char *in_file = NULL;
    const char *out_file = NULL;

    /* Check if invoked via symlink name */
    if (strstr(prog, "encrypt")) {
        mode = "encrypt";
        in_file = (argc > 1) ? argv[1] : "cramfs.img";
        out_file = (argc > 2) ? argv[2] : "iroot_encrypted.bin";
    } else if (strstr(prog, "decrypt")) {
        mode = "decrypt";
        in_file = (argc > 1) ? argv[1] : "SA5212M4_BMC_4.35.0_Standard_20191025";
        out_file = (argc > 2) ? argv[2] : "cramfs.img";
    } else if (argc >= 2) {
        if (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "encrypt") == 0) {
            mode = "encrypt";
            in_file = (argc > 2) ? argv[2] : "cramfs.img";
            out_file = (argc > 3) ? argv[3] : "iroot_encrypted.bin";
        } else if (strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "decrypt") == 0) {
            mode = "decrypt";
            in_file = (argc > 2) ? argv[2] : "SA5212M4_BMC_4.35.0_Standard_20191025";
            out_file = (argc > 3) ? argv[3] : "cramfs.img";
        }
    }

    if (!mode) {
        fprintf(stderr, "Usage:\\n");
        fprintf(stderr, "  %s encrypt <cramfs_in> <iroot_enc_out>\\n", prog);
        fprintf(stderr, "  %s decrypt <rom_or_iroot_in> <cramfs_out>\\n", prog);
        return 1;
    }

    if (strcmp(mode, "encrypt") == 0) {
        return do_encrypt(in_file, out_file);
    } else {
        return do_decrypt(in_file, out_file);
    }
}
