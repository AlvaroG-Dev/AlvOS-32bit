#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Define the keyboard_layout_t structure (from keyboard.h)
typedef struct {
    char name[32];
    uint8_t normal[128];
    uint8_t shift[128];
    uint8_t altgr[128];
} keyboard_layout_t;

// Define KEYBOARD_LAYOUT_MAGIC (from keyboard_driver.h)
#define KEYBOARD_LAYOUT_MAGIC 0x4B4244

// Spanish QWERTY layout arrays (using Latin-1 values for special characters)
static const uint8_t es_qwerty_normal[128] = {
    [0x00] = 0,      [0x01] = 27,     [0x02] = '1',    [0x03] = '2',    // ESC, 1, 2
    [0x04] = '3',    [0x05] = '4',    [0x06] = '5',    [0x07] = '6',    // 3, 4, 5, 6
    [0x08] = '7',    [0x09] = '8',    [0x0A] = '9',    [0x0B] = '0',    // 7, 8, 9, 0
    [0x0C] = '\'',   [0x0D] = 0xA1,   [0x0E] = '\b',   [0x0F] = '\t',   // ', ¡, Backspace, Tab
    [0x10] = 'q',    [0x11] = 'w',    [0x12] = 'e',    [0x13] = 'r',    // q, w, e, r
    [0x14] = 't',    [0x15] = 'y',    [0x16] = 'u',    [0x17] = 'i',    // t, y, u, i
    [0x18] = 'o',    [0x19] = 'p',    [0x1A] = '`',    [0x1B] = '+',    // o, p, `, +
    [0x1C] = '\n',   [0x1D] = 0,      [0x1E] = 'a',    [0x1F] = 's',    // Enter, Left Ctrl, a, s
    [0x20] = 'd',    [0x21] = 'f',    [0x22] = 'g',    [0x23] = 'h',    // d, f, g, h
    [0x24] = 'j',    [0x25] = 'k',    [0x26] = 'l',    [0x27] = 0xF1,   // j, k, l, ñ
    [0x28] = 0xB4,   [0x29] = 0xBA,   [0x2A] = 0,      [0x2B] = 0xE7,   // ´, º, Left Shift, ç
    [0x2C] = 'z',    [0x2D] = 'x',    [0x2E] = 'c',    [0x2F] = 'v',    // z, x, c, v
    [0x30] = 'b',    [0x31] = 'n',    [0x32] = 'm',    [0x33] = ',',    // b, n, m, ,
    [0x34] = '.',    [0x35] = '-',    [0x36] = 0,      [0x37] = '*',    // ., -, Right Shift, *
    [0x38] = 0,      [0x39] = ' ',    [0x3A] = 0,      [0x3B] = 0,      // Left Alt, Space, Caps Lock, F1
    [0x3C] = 0,      [0x3D] = 0,      [0x3E] = 0,      [0x3F] = 0,      // F2, F3, F4, F5
    [0x40] = 0,      [0x41] = 0,      [0x42] = 0,      [0x43] = 0,      // F6, F7, F8, F9
    [0x44] = 0,      [0x45] = 0,      [0x46] = 0,      [0x47] = 0,      // F10, Num Lock, Scroll Lock, KP7
    [0x48] = 0,      [0x49] = 0,      [0x4A] = '-',    [0x4B] = 0,      // KP8, KP9, KP-, KP4
    [0x4C] = 0,      [0x4D] = '+',    [0x4E] = 0,      [0x4F] = 0,      // KP5, KP6, KP+, KP1
    [0x50] = 0,      [0x51] = 0,      [0x52] = 0,      [0x53] = 0,      // KP2, KP3, KP0, KP.
    [0x54 ... 0x7F] = 0              // Remaining scancodes mapped to 0
};

static const uint8_t es_qwerty_shift[128] = {
    [0x00] = 0,      [0x01] = 27,     [0x02] = '!',    [0x03] = '"',    // ESC, !, "
    [0x04] = 0xB7,   [0x05] = '$',    [0x06] = '%',    [0x07] = '&',    // ·, $, %, &
    [0x08] = '/',    [0x09] = '(',    [0x0A] = ')',    [0x0B] = '=',    // /, (, ), =
    [0x0C] = '?',    [0x0D] = 0xBF,   [0x0E] = '\b',   [0x0F] = '\t',   // ?, ¿, Backspace, Tab
    [0x10] = 'Q',    [0x11] = 'W',    [0x12] = 'E',    [0x13] = 'R',    // Q, W, E, R
    [0x14] = 'T',    [0x15] = 'Y',    [0x16] = 'U',    [0x17] = 'I',    // T, Y, U, I
    [0x18] = 'O',    [0x19] = 'P',    [0x1A] = '^',    [0x1B] = '*',    // O, P, ^, *
    [0x1C] = '\n',   [0x1D] = 0,      [0x1E] = 'A',    [0x1F] = 'S',    // Enter, Left Ctrl, A, S
    [0x20] = 'D',    [0x21] = 'F',    [0x22] = 'G',    [0x23] = 'H',    // D, F, G, H
    [0x24] = 'J',    [0x25] = 'K',    [0x26] = 'L',    [0x27] = 0xD1,   // J, K, L, Ñ
    [0x28] = 0xA8,   [0x29] = 0xAA,   [0x2A] = 0,      [0x2B] = 0xC7,   // ¨, ª, Left Shift, Ç
    [0x2C] = 'Z',    [0x2D] = 'X',    [0x2E] = 'C',    [0x2F] = 'V',    // Z, X, C, V
    [0x30] = 'B',    [0x31] = 'N',    [0x32] = 'M',    [0x33] = ';',    // B, N, M, ;
    [0x34] = ':',    [0x35] = '_',    [0x36] = 0,      [0x37] = '*',    // :, _, Right Shift, *
    [0x38] = 0,      [0x39] = ' ',    [0x3A] = 0,      [0x3B] = 0,      // Left Alt, Space, Caps Lock, F1
    [0x3C ... 0x7F] = 0              // Remaining scancodes mapped to 0
};

static const uint8_t es_qwerty_altgr[128] = {
    [0x00] = 0,      [0x01] = 0,      [0x02] = '|',    [0x03] = '@',    // |, @
    [0x04] = '#',    [0x05] = '~',    [0x06] = 0x80,   [0x07] = 0,      // #, ~, €, 0
    [0x08] = 0,      [0x09] = 0,      [0x0A] = 0,      [0x0B] = 0,      // 0, 0, 0, 0
    [0x0C] = '\\',   [0x0D] = 0,      [0x0E] = 0,      [0x0F] = 0,      // \, 0, 0, 0
    [0x10] = 0,      [0x11] = 0,      [0x12] = 0,      [0x13] = 0,      // q, w, e, r
    [0x14] = 0,      [0x15] = 0,      [0x16] = 0,      [0x17] = 0,      // t, y, u, i
    [0x18] = 0,      [0x19] = 0,      [0x1A] = '[',    [0x1B] = ']',    // o, p, [, ]
    [0x1C] = 0,      [0x1D] = 0,      [0x1E] = 0,      [0x1F] = 0,      // Enter, Left Ctrl, a, s
    [0x20] = 0,      [0x21] = 0,      [0x22] = 0,      [0x23] = 0,      // d, f, g, h
    [0x24] = 0,      [0x25] = 0,      [0x26] = 0,      [0x27] = 0,      // j, k, l, ñ
    [0x28] = '{',    [0x29] = '}',    [0x2A] = 0,      [0x2B] = 0,      // {, }, Left Shift, ç
    [0x2C] = 0,      [0x2D] = 0,      [0x2E] = 0,      [0x2F] = 0,      // z, x, c, v
    [0x30] = 0,      [0x31] = 0,      [0x32] = 0,      [0x33] = 0,      // b, n, m, ,
    [0x34] = 0,      [0x35] = 0,      [0x36] = 0,      [0x37] = 0,      // ., -, Right Shift, *
    [0x38] = 0,      [0x39] = 0,      [0x3A ... 0x7F] = 0 // Left Alt, Space, Caps Lock, etc.
};

int main() {
    // Create the keyboard_layout_t structure
    keyboard_layout_t layout = {
        .name = "ES-QWERTY",
        .normal = {0},
        .shift = {0},
        .altgr = {0}
    };
    memcpy(layout.normal, es_qwerty_normal, sizeof(es_qwerty_normal));
    memcpy(layout.shift, es_qwerty_shift, sizeof(es_qwerty_shift));
    memcpy(layout.altgr, es_qwerty_altgr, sizeof(es_qwerty_altgr));

    // Open output file
    FILE *fp = fopen("es-kbd.kbd", "wb");
    if (!fp) {
        printf("Error: Could not open es-kbd.kbd for writing\n");
        return 1;
    }

    // Write KEYBOARD_LAYOUT_MAGIC
    uint32_t magic = KEYBOARD_LAYOUT_MAGIC;
    fwrite(&magic, sizeof(magic), 1, fp);

    // Write the layout structure
    fwrite(&layout, sizeof(keyboard_layout_t), 1, fp);

    fclose(fp);
    printf("Successfully created es-kbd.kbd\n");
    return 0;
}