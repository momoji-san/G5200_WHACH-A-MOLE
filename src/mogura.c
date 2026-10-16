#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

#define FBDEV "/dev/fb1"
#define TOUCHDEV "/dev/input/event0"
#define W 240
#define H 120
#define HOLES 9
#define GAME_SECONDS 10
#define MOLE_INTERVAL_MIN_NS 700000000L
#define MOLE_INTERVAL_MAX_NS 1200000000L
#define HIT_FACE_NS 350000000L
#define START_X 169
#define START_Y 5
#define START_W 66
#define START_H 22

static int fbfd = -1, touchfd = -1;
static uint32_t *fb;
static size_t fbsize;
static int stride_pixels = W;
static int touch_x = 120, touch_y = 60;
static int touch_down = 0;
static int raw_x = 2000, raw_y = 2000;
static int score = 0, mole = -1, hit_mole = -1;
static struct timespec hit_until;
static int state = 0;
static time_t game_end;
static struct timespec next_mole;
static struct timespec last_refresh;
static int refresh_pending = 0;
static struct fb_var_screeninfo fb_vinfo;
static int fb_vinfo_valid = 0;
static int last_seconds = -1;

#define MIN_REFRESH_INTERVAL_NS 550000000L

static const char *glyph(char c) {
    switch (c) {
    case 'A': return "01110100011000111111100011000110001";
    case 'B': return "11110100011000111110100011000111110";
    case 'C': return "01111100001000010000100001000001111";
    case 'D': return "11110100011000110001100011000111110";
    case 'E': return "11111100001000011110100001000011111";
    case 'F': return "11111100001000011110100001000010000";
    case 'G': return "01111100001000010111100011000101110";
    case 'H': return "10001100011000111111100011000110001";
    case 'I': return "11111001000010000100001000010011111";
    case 'J': return "00111000100001000010000101001001100";
    case 'K': return "10001100101010011000101001001010001";
    case 'L': return "10000100001000010000100001000011111";
    case 'M': return "10001110111010110101100011000110001";
    case 'N': return "10001110011010110011100011000110001";
    case 'O': return "01110100011000110001100011000101110";
    case 'P': return "11110100011000111110100001000010000";
    case 'Q': return "01110100011000110001101011001001101";
    case 'R': return "11110100011000111110101001001010001";
    case 'S': return "01111100001000001110000010000111110";
    case 'T': return "11111001000010000100001000010000100";
    case 'U': return "10001100011000110001100011000101110";
    case 'V': return "10001100011000110001100010101000100";
    case 'W': return "10001100011000110101101011101110001";
    case 'X': return "10001100010101000100010101000110001";
    case 'Y': return "10001100010101000100001000010000100";
    case 'Z': return "11111000010001000100010001000011111";
    case '0': return "01110100011001110101110011000101110";
    case '1': return "00100011000010000100001000010001110";
    case '2': return "01110100010000100010001000100011111";
    case '3': return "11110000010000101110000010000111110";
    case '4': return "00010001100101010010111110001000010";
    case '5': return "11111100001000011110000010000111110";
    case '6': return "01110100001000011110100011000101110";
    case '7': return "11111000010001000100010000100001000";
    case '8': return "01110100011000101110100011000101110";
    case '9': return "01110100011000101111000010000101110";
    case ':': return "00000001000001000000000000100000100";
    case '-': return "00000000000000011111000000000000000";
    default:  return "00000000000000000000000000000000000";
    }
}

static void pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < W && y >= 0 && y < H)
        fb[y * stride_pixels + x] = color;
}
static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    int xx, yy;
    for (yy = y; yy < y + h; yy++)
        for (xx = x; xx < x + w; xx++)
            pixel(xx, yy, c);
}
static void rect(int x, int y, int w, int h, uint32_t c) {
    int i;
    for (i = 0; i < w; i++) {
        pixel(x + i, y, c);
        pixel(x + i, y + h - 1, c);
    }
    for (i = 0; i < h; i++) {
        pixel(x, y + i, c);
        pixel(x + w - 1, y + i, c);
    }
}
static void text(int x, int y, const char *s, int scale) {
    for (; *s; s++, x += 6 * scale) {
        const char *g = glyph(*s);
        int xx, yy;
        for (yy = 0; yy < 7; yy++)
            for (xx = 0; xx < 5; xx++)
                if (g[yy * 5 + xx] == '1')
                    fill_rect(x + xx * scale, y + yy * scale, scale, scale, 0x00000000);
    }
}
static void ellipse(int cx, int cy, int rx, int ry, uint32_t color, int filled) {
    int x, y;
    for (y = -ry; y <= ry; y++) {
        for (x = -rx; x <= rx; x++) {
            long lhs = (long)x * x * ry * ry + (long)y * y * rx * rx;
            long rhs = (long)rx * rx * ry * ry;
            if ((filled && lhs <= rhs) ||
                (!filled && lhs <= rhs &&
                 lhs >= (long)(rx - 2) * (rx - 2) * (ry - 2) * (ry - 2)))
                pixel(cx + x, cy + y, color);
        }
    }
}
static void clear_screen(void) {
    fill_rect(0, 0, W, H, 0xffffffff);
}

static void draw_hole(int i, int with_mole, int hit) {
    int col = i % 3, row = i / 3;
    int cx = 40 + col * 80;
    int cy = 43 + row * 30;

    ellipse(cx, cy, 19, 8, 0x00000000, 1);
    ellipse(cx, cy - 2, 15, 4, 0xffffffff, 1);

    if (with_mole) {
        /* 少し低め・小さめにして重なりを緩和 */
        ellipse(cx, cy - 13, 13, 13, 0x00000000, 1);
        fill_rect(cx - 13, cy - 13, 27, 13, 0x00000000);

        ellipse(cx, cy - 10, 9, 5, 0xffffffff, 1);

        if (hit) {
            for (int k = 0; k < 7; k++) {
                for (int t = -1; t <= 1; t++) {
                    pixel(cx - 8 + k, cy - 24 + k + t, 0xffffffff);
                    pixel(cx - 8 + k, cy - 18 - k + t, 0xffffffff);
                    pixel(cx + 1 + k, cy - 24 + k + t, 0xffffffff);
                    pixel(cx + 1 + k, cy - 18 - k + t, 0xffffffff);
                }
            }
            pixel(cx - 5, cy - 11, 0x00000000);
            pixel(cx - 4, cy - 10, 0x00000000);
            pixel(cx - 3, cy - 9,  0x00000000);
            pixel(cx - 2, cy - 8,  0x00000000);
            pixel(cx - 1, cy - 7,  0x00000000);
            pixel(cx,     cy - 7,  0x00000000);
            pixel(cx + 1, cy - 7,  0x00000000);
            pixel(cx + 2, cy - 8,  0x00000000);
            pixel(cx + 3, cy - 9,  0x00000000);
            pixel(cx + 4, cy - 10, 0x00000000);
            pixel(cx + 5, cy - 11, 0x00000000);
        } else {
            ellipse(cx - 5, cy - 20, 3, 3, 0xffffffff, 1);
            ellipse(cx + 5, cy - 20, 3, 3, 0xffffffff, 1);
            ellipse(cx - 5, cy - 20, 1, 1, 0x00000000, 1);
            ellipse(cx + 5, cy - 20, 1, 1, 0x00000000, 1);

            pixel(cx - 3, cy - 11, 0x00000000);
            pixel(cx - 2, cy - 10, 0x00000000);
            pixel(cx - 1, cy - 9,  0x00000000);
            pixel(cx,     cy - 9,  0x00000000);
            pixel(cx + 1, cy - 9,  0x00000000);
            pixel(cx + 2, cy - 10, 0x00000000);
            pixel(cx + 3, cy - 11, 0x00000000);
        }

        for (int k = 0; k < 3; k++) {
            int yy = cy - 12 + k * 2;
            for (int d = 1; d <= 5; d++) {
                pixel(cx - 7 - d, yy + (k == 0 ? -d / 4 : (k == 2 ? d / 4 : 0)), 0x00000000);
                pixel(cx + 7 + d, yy + (k == 0 ? -d / 4 : (k == 2 ? d / 4 : 0)), 0x00000000);
            }
        }
    }
}

static void refresh(void) {
    refresh_pending = 1;
}
static void flush_refresh(void) {
    struct timespec now;
    long elapsed_ns;
    if (!refresh_pending) return;

    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed_ns = (now.tv_sec - last_refresh.tv_sec) * 1000000000L +
                 (now.tv_nsec - last_refresh.tv_nsec);
    if (last_refresh.tv_sec != 0 && elapsed_ns < MIN_REFRESH_INTERVAL_NS)
        return;

    if (!fb_vinfo_valid) return;

    /* 失敗してもプログラムを止めない（SPIタイムアウト対策） */
    if (ioctl(fbfd, FBIOPAN_DISPLAY, &fb_vinfo) < 0) {
        /* エラーを無視して続行 */
        refresh_pending = 0;
        return;
    }
    last_refresh = now;
    refresh_pending = 0;
}

static void draw_header(int seconds) {
    char buf[32];
    fill_rect(0, 0, W, 15, 0xffffffff);
    snprintf(buf, sizeof(buf), "SCORE %d", score);
    text(3, 4, buf, 1);
    snprintf(buf, sizeof(buf), "TIME %02d", seconds);
    text(155, 4, buf, 1);
}

static long random_mole_interval_ns(void) {
    return MOLE_INTERVAL_MIN_NS +
           (long)(rand() % (int)(MOLE_INTERVAL_MAX_NS - MOLE_INTERVAL_MIN_NS + 1));
}
static void set_next_mole_time(struct timespec base) {
    next_mole = base;
    next_mole.tv_nsec += random_mole_interval_ns();
    if (next_mole.tv_nsec >= 1000000000L) {
        next_mole.tv_sec++;
        next_mole.tv_nsec -= 1000000000L;
    }
}
static int random_mole_except(int previous) {
    int n = rand() % (HOLES - 1);
    if (n >= previous) n++;
    return n;
}
static void draw_start_button(void) {
    rect(START_X, START_Y, START_W, START_H, 0x00000000);
    text(START_X + (START_W - 30) / 2, START_Y + 7, "START", 1);
}
static int start_button_tapped(void) {
    return touch_x >= START_X && touch_x < START_X + START_W &&
           touch_y >= START_Y && touch_y < START_Y + START_H;
}

static void draw_title(void) {
    clear_screen();
    rect(3, 3, W - 6, H - 6, 0x00000000);
    text(8, 28, "WHACK-A-MOLE", 2);
    draw_start_button();
    draw_hole(6, 1, 0);
    draw_hole(7, 1, 0);
    draw_hole(8, 1, 0);
    last_seconds = -1;
    refresh();
}

static void draw_game(int seconds) {
    int i;
    clear_screen();
    draw_header(seconds);
    for (i = 0; i < HOLES; i++)
        draw_hole(i, (i == mole || i == hit_mole), i == hit_mole);
    last_seconds = seconds;
    refresh();
}

static void draw_over(void) {
    char buf[32];
    clear_screen();
    rect(3, 3, W - 6, H - 6, 0x00000000);
    text(8, 28, "WHACK-A-MOLE", 2);
    snprintf(buf, sizeof(buf), "GAME OVER  SCORE %d", score);
    text((W - (int)strlen(buf) * 6) / 2, 53, buf, 1);
    draw_start_button();
    draw_hole(6, 1, 0);
    draw_hole(7, 1, 0);
    draw_hole(8, 1, 0);
    last_seconds = -1;
    refresh();
}

static void start_game(void) {
    score = 0;
    state = 1;
    mole = rand() % HOLES;
    hit_mole = -1;
    game_end = time(NULL) + GAME_SECONDS;
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        set_next_mole_time(now);
    }
    draw_game(GAME_SECONDS);
}

static void update_touch_coords(void) {
    int x = (raw_x - 228) * (W - 1) / (4015 - 228);
    int y = (3817 - raw_y) * (H - 1) / (3817 - 421);
    if (x < 0) x = 0;
    if (x >= W) x = W - 1;
    if (y < 0) y = 0;
    if (y >= H) y = H - 1;
    touch_x = x;
    touch_y = y;
}

static void handle_tap(void) {
    if (state == 0) {
        if (start_button_tapped()) start_game();
        return;
    }
    if (state == 2) {
        if (start_button_tapped()) start_game();
        return;
    }
    if (state == 1) {
        int col = touch_x / 80;
        int row = (touch_y - 28) / 30;
        if (col < 0) col = 0;
        if (col > 2) col = 2;
        if (row < 0) row = 0;
        if (row > 2) row = 2;
        int idx = row * 3 + col;
        if (touch_y >= 25 && touch_y < 115 && idx == mole) {
            struct timespec now;
            score += 10;
            hit_mole = mole;
            mole = -1;
            clock_gettime(CLOCK_MONOTONIC, &now);
            hit_until = now;
            hit_until.tv_nsec += HIT_FACE_NS;
            if (hit_until.tv_nsec >= 1000000000L) {
                hit_until.tv_sec++;
                hit_until.tv_nsec -= 1000000000L;
            }
            draw_game((int)(game_end - time(NULL)));
            last_refresh.tv_sec = 0;
            flush_refresh();
        }
    }
}

static int time_reached(struct timespec a, struct timespec b) {
    return a.tv_sec > b.tv_sec ||
           (a.tv_sec == b.tv_sec && a.tv_nsec >= b.tv_nsec);
}

static int setup_fb(void) {
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    fbfd = open(FBDEV, O_RDWR);
    if (fbfd < 0) {
        perror("open " FBDEV);
        return -1;
    }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) < 0 ||
        ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("framebuffer ioctl");
        return -1;
    }
    fb_vinfo = var;
    fb_vinfo_valid = 1;
    if (var.xres < W || var.yres < H || var.bits_per_pixel != 32) {
        fprintf(stderr, "Expected at least %dx%d at 32 bpp; got %ux%u, %u bpp\n",
                W, H, var.xres, var.yres, var.bits_per_pixel);
        return -1;
    }
    stride_pixels = fix.line_length / 4;
    fbsize = fix.smem_len;
    fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap framebuffer");
        fb = NULL;
        return -1;
    }
    return 0;
}

int main(void) {
    struct pollfd pfd;
    struct input_event ev;

    srand((unsigned int)(time(NULL) ^ getpid()));
    if (setup_fb() < 0) return 1;

    touchfd = open(TOUCHDEV, O_RDONLY | O_NONBLOCK);
    if (touchfd < 0) {
        perror("open " TOUCHDEV);
        munmap(fb, fbsize);
        close(fbfd);
        return 1;
    }

    pfd.fd = touchfd;
    pfd.events = POLLIN;
    draw_title();
    fprintf(stderr, "Mogura-tataki running. Press Ctrl+C to quit.\n");

    for (;;) {
        int pr = poll(&pfd, 1, 60);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            int events_processed = 0;
            while (events_processed < 32 &&
                   read(touchfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                events_processed++;
                if (ev.type == EV_ABS) {
                    if (ev.code == ABS_X) raw_x = ev.value;
                    else if (ev.code == ABS_Y) raw_y = ev.value;
                    update_touch_coords();
                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    if (ev.value == 1 && !touch_down) {
                        touch_down = 1;
                        handle_tap();
                    } else if (ev.value == 0) {
                        touch_down = 0;
                    }
                }
            }
        }

        flush_refresh();

        if (state == 1) {
            time_t now = time(NULL);
            int sec = (int)(game_end - now);
            if (sec <= 0) {
                state = 2;
                mole = -1;
                hit_mole = -1;
                draw_over();
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                if (hit_mole >= 0 && time_reached(ts, hit_until)) {
                    int previous = hit_mole;
                    hit_mole = -1;
                    mole = random_mole_except(previous);
                    set_next_mole_time(ts);
                    draw_game(sec);
                } else if (mole >= 0 && time_reached(ts, next_mole)) {
                    int previous = mole;
                    mole = random_mole_except(previous);
                    set_next_mole_time(ts);
                    draw_game(sec);
                } else if (sec != last_seconds) {
                    draw_game(sec);
                }
            }
        }
    }

    return 0;
}
