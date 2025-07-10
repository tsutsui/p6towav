#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#define WAV_HEADER_SIZE 44
#define SAMPLE_RATE 19200

#define TAPE_BAUD 1200
#define BIT0_FREQ TAPE_BAUD
#define BIT1_FREQ (TAPE_BAUD * 2)

#if (SAMPLE_RATE % (TAPE_BAUD * 2)) != 0
#error SAMPLE_RATE should be multiple of TAPE_BAUD * 2 
#endif

#define BIT_SAMPLES (SAMPLE_RATE / TAPE_BAUD)

#define LONG_HEADER_MSEC 2000
#define SHORT_HEADER_MSEC 500

#define BASIC_HEADER_SIZE 16
#define BASIC_HEADER_BIN 0xd3
#define BASIC_NAME_LEN 6
#define BASIC_HEADER_LEN (BASIC_HEADER_SIZE - BASIC_NAME_LEN)

#define STOP_BITS 3

/* WAVヘッダ書き込み用 little endian変換 */
static inline void
int_to_le32enc(uint8_t *buf, uint32_t in)
{
    buf[0] =  in        & 0xff;
    buf[1] = (in >>  8) & 0xff;
    buf[2] = (in >> 16) & 0xff;
    buf[3] = (in >> 24) & 0xff;
}

/* WAVヘッダの書き出し */
static void
write_wav_header(FILE *f, uint32_t sample_count)
{
    uint8_t file_size_le[4];
    uint8_t smpl_rate_le[4];
    uint8_t byte_rate_le[4];
    uint8_t data_size_le[4];
    uint32_t smpl_rate = SAMPLE_RATE;
    uint32_t byte_rate = SAMPLE_RATE; /* for mono 8 bit PCM */
    uint32_t data_size = sample_count;
    uint32_t file_size = data_size + WAV_HEADER_SIZE - 8;

    int_to_le32enc(file_size_le, file_size);
    int_to_le32enc(smpl_rate_le, smpl_rate);
    int_to_le32enc(byte_rate_le, byte_rate);
    int_to_le32enc(data_size_le, data_size);

    /* エンディアンやアラインメント判定が面倒なので uint8_t ベタ書き */
    uint8_t header[WAV_HEADER_SIZE] = {
        'R','I','F','F',        /* char[4]: magic */
        file_size_le[0], file_size_le[1], file_size_le[2], file_size_le[3],
        'W','A','V','E',        /* char[4]: magic */
        'f','m','t',' ',        /* char[4]: fmt id */
        16, 0, 0, 0,            /* uint32_t: format size (16 for PCM) */
        1, 0,                   /* uint16_t: audio format: (1: PCM) */
        1, 0,                   /* uint16_t: number of channel (1: mono) */
        smpl_rate_le[0], smpl_rate_le[1], smpl_rate_le[2], smpl_rate_le[3],
        byte_rate_le[0], byte_rate_le[1], byte_rate_le[2], byte_rate_le[3],
        1, 0,                   /* uint16_t: block align */
        8, 0,                   /* uint16_t: bits per sample */
        'd','a','t','a',        /* char[4]: data id */
        data_size_le[0], data_size_le[1], data_size_le[2], data_size_le[3]
    };

    fwrite(header, 1, WAV_HEADER_SIZE, f);
}

/* 矩形波の出力（8bit PCM） */
static void
write_square_wave(FILE *f, uint32_t freq, int total_samples)
{
    int samples_per_half = SAMPLE_RATE / (freq * 2);
    uint8_t value = 0xFF;

    for (int i = 0, count = 0; i < total_samples; i++) {
        fwrite(&value, 1, 1, f);
        if (++count >= samples_per_half) {
            value = ~value;
            count = 0;
        }
    }
}

/* ビット出力（0 or 1） */
static void
write_bit(FILE *f, int bit)
{
    uint32_t freq = bit ? BIT1_FREQ : BIT0_FREQ;
    write_square_wave(f, freq, BIT_SAMPLES);
}

/* UART バイト出力 */
static void
write_byte(FILE *f, uint8_t byte)
{
    /* start bit (1 bit) */
    write_bit(f, 0);
    /* byte data */
    for (int i = 0; i < 8; i++) {
        write_bit(f, (byte >> i) & 1);
    }
    /* stop bits */
    for (int i = 0; i < STOP_BITS; i++) {
       write_bit(f, 1);
    }
}

/* データブロック出力（ヘッダ付き） */
static void
write_block(FILE *f, const uint8_t *data, size_t len, int short_header)
{
    uint32_t header_msec = short_header ? SHORT_HEADER_MSEC : LONG_HEADER_MSEC;
    uint32_t header_samples = SAMPLE_RATE * header_msec / 1000;
    write_square_wave(f, BIT1_FREQ, header_samples);
    for (int i = 0; i < len; ++i) {
        write_byte(f, data[i]);
    }
}

/* BASIC形式の判定 */
static int
is_basic_format(const uint8_t *buf, size_t size)
{
    if (size < BASIC_HEADER_SIZE)
        return 0;
    for (int i = 0; i < BASIC_HEADER_LEN; i++) {
        if (buf[i] != BASIC_HEADER_BIN)
            return 0;
    }
    return 1;
}

int
main(int argc, char *argv[])
{
    int status = EXIT_FAILURE;
    FILE *p6file = NULL;
    FILE *wavefile = NULL;
    uint8_t *buf = NULL;

    if (argc != 3) {
        errx(EXIT_FAILURE, "usage: %s input.p6 output.wav\n", argv[0]);
    }

    p6file = fopen(argv[1], "rb");
    if (p6file == NULL) {
        perror("fopen p6file");
        goto out;
    }

    fseek(p6file, 0, SEEK_END);
    size_t p6size = ftell(p6file);
    fseek(p6file, 0, SEEK_SET);

    buf = malloc(p6size);
    if (buf == NULL) {
        perror("malloc");
        goto out;
    }

    if (fread(buf, 1, p6size, p6file) != p6size) {
        perror("fread p6file");
        goto out;
    }

    wavefile = fopen(argv[2], "wb");
    if (wavefile == NULL) {
        perror("fopen wavefile");
        goto out;
    }

    /* wavヘッダは書き込み終わってサイズ確定後に書くのでいったんスキップ */
    fseek(wavefile, WAV_HEADER_SIZE, SEEK_SET);

    if (is_basic_format(buf, p6size)) {
        /* BASIC形式：10バイト同期 + 6バイトファイル名 + 本体 */
        char filename[BASIC_NAME_LEN + 1] = { 0 };
        memcpy(filename, buf + BASIC_HEADER_LEN, BASIC_NAME_LEN);
        fprintf(stderr, "BASIC file \"%s\"\n", filename);

        /* ロングヘッダ + BASICヘッダ */
        write_block(wavefile, buf, BASIC_HEADER_SIZE, 0);
        if (p6size > BASIC_HEADER_SIZE) {
            /* ショートヘッダ + BASIC本体 */
            write_block(wavefile,
              buf + BASIC_HEADER_SIZE, p6size - BASIC_HEADER_SIZE, 1);
        }
    } else {
        /* その他形式（ロングヘッダ + データで出力） */
        fprintf(stderr, "binary data\n");
        write_block(wavefile, buf, p6size, 0);
    }

    long file_end = ftell(wavefile);
    uint32_t sample_count = (uint32_t)file_end - WAV_HEADER_SIZE;
    fseek(wavefile, 0, SEEK_SET);
    write_wav_header(wavefile, sample_count);
    status = EXIT_SUCCESS;

 out:
    if (wavefile != NULL)
        fclose(wavefile);
    if (buf != NULL)
        free(buf);
    if (p6file != NULL)
        fclose(p6file);

    exit(status);
}
