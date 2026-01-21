#include "lib_os.h"

#define MAX_LINES 1000
#define MAX_LINE_LEN 160
#define STATUS_HEIGHT 1

typedef struct {
  char data[MAX_LINE_LEN];
  int len;
} line_t;

typedef struct {
  line_t lines[MAX_LINES];
  int line_count;
  int cursor_x, cursor_y;
  int scroll_y;
  char filename[128];
  bool dirty;
  int term_w, term_h;
} editor_t;

editor_t ed;

void term_goto(int x, int y) {
  char buf[32];
  int i = 0;
  buf[i++] = '\x1b';
  buf[i++] = '[';

  int val = y + 1;
  if (val >= 10)
    buf[i++] = (val / 10) + '0';
  buf[i++] = (val % 10) + '0';
  buf[i++] = ';';

  val = x + 1;
  if (val >= 100)
    buf[i++] = (val / 100) + '0';
  if (val >= 10)
    buf[i++] = ((val / 10) % 10) + '0';
  buf[i++] = (val % 10) + '0';
  buf[i++] = 'H';
  buf[i++] = '\0';
  print(buf);
}

void editor_draw() {
  print(ANSI_CLEAR);
  for (int i = 0; i < ed.term_h - STATUS_HEIGHT; i++) {
    int idx = i + ed.scroll_y;
    if (idx < ed.line_count) {
      write(1, ed.lines[idx].data, ed.lines[idx].len);
    }
    if (i < ed.term_h - STATUS_HEIGHT - 1)
      print("\r\n");
  }

  // Status bar
  term_goto(0, ed.term_h - 1);
  print(ANSI_REVERSE);

  rtc_time_t t;
  get_rtc(&t);

  char status[256];
  int len = 0;
  const char *fn = ed.filename[0] ? ed.filename : "[No Name]";
  while (fn[len] && len < 64) {
    status[len] = fn[len];
    len++;
  }
  if (ed.dirty)
    status[len++] = '*';
  status[len++] = ' ';
  status[len++] = '-';
  status[len++] = ' ';

  status[len++] = (t.hour / 10) + '0';
  status[len++] = (t.hour % 10) + '0';
  status[len++] = ':';
  status[len++] = (t.minute / 10) + '0';
  status[len++] = (t.minute % 10) + '0';
  status[len++] = ' ';

  status[len++] = 'L';
  int y = ed.cursor_y + 1;
  if (y >= 100)
    status[len++] = (y / 100) + '0';
  if (y >= 10)
    status[len++] = ((y / 10) % 10) + '0';
  status[len++] = (y % 10) + '0';

  const char *help = "  ^S:Save ^X:Exit";
  int h_len = 0;
  while (help[h_len])
    h_len++;

  int remaining = ed.term_w - len - h_len;
  while (remaining > 0) {
    status[len++] = ' ';
    remaining--;
  }

  h_len = 0;
  while (help[h_len]) {
    status[len++] = help[h_len++];
  }

  status[len] = '\0';
  write(1, status, len);
  print(ANSI_RESET);

  term_goto(ed.cursor_x, ed.cursor_y - ed.scroll_y);
}

void editor_load(const char *filename) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    ed.line_count = 1;
    ed.lines[0].len = 0;
    return;
  }

  char c;
  int line = 0;
  int pos = 0;
  while (read(fd, &c, 1) > 0 && line < MAX_LINES) {
    if (c == '\n') {
      ed.lines[line].len = pos;
      line++;
      pos = 0;
    } else if (c != '\r') {
      if (pos < MAX_LINE_LEN) {
        ed.lines[line].data[pos++] = c;
      }
    }
  }
  if (pos > 0 || line == 0) {
    ed.lines[line].len = pos;
    line++;
  }
  ed.line_count = line;
  close(fd);
  ed.dirty = false;
}

void editor_save() {
  if (!ed.filename[0])
    return;
  int fd = open(ed.filename, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0)
    return;

  for (int i = 0; i < ed.line_count; i++) {
    write(fd, ed.lines[i].data, ed.lines[i].len);
    if (i < ed.line_count - 1) {
      char n = '\n';
      write(fd, &n, 1);
    }
  }
  close(fd);
  ed.dirty = false;
}

int main_entry(int argc, char **argv) {
  ed.term_w = 80;
  ed.term_h = 24; // Standard height
  ed.line_count = 0;
  ed.cursor_x = 0;
  ed.cursor_y = 0;
  ed.scroll_y = 0;
  ed.dirty = false;
  ed.filename[0] = '\0';

  if (argc > 1) {
    int i = 0;
    while (argv[1][i] && i < 127) {
      ed.filename[i] = argv[1][i];
      i++;
    }
    ed.filename[i] = '\0';
    editor_load(ed.filename);
  } else {
    ed.line_count = 1;
    ed.lines[0].len = 0;
  }

  while (1) {
    editor_draw();
    int k = readkey();

    if (k == 24)
      break; // Ctrl+X
    if (k == 19) {
      editor_save();
      continue;
    } // Ctrl+S

    if (k == KEY_UP) {
      if (ed.cursor_y > 0)
        ed.cursor_y--;
    } else if (k == KEY_DOWN) {
      if (ed.cursor_y < ed.line_count - 1)
        ed.cursor_y++;
    } else if (k == KEY_LEFT) {
      if (ed.cursor_x > 0)
        ed.cursor_x--;
    } else if (k == KEY_RIGHT) {
      if (ed.cursor_x < ed.lines[ed.cursor_y].len)
        ed.cursor_x++;
    } else if (k == '\n' || k == '\r') {
      if (ed.line_count < MAX_LINES) {
        for (int i = ed.line_count; i > ed.cursor_y + 1; i--) {
          ed.lines[i] = ed.lines[i - 1];
        }
        int remaining = ed.lines[ed.cursor_y].len - ed.cursor_x;
        ed.lines[ed.cursor_y + 1].len = remaining;
        for (int i = 0; i < remaining; i++) {
          ed.lines[ed.cursor_y + 1].data[i] =
              ed.lines[ed.cursor_y].data[ed.cursor_x + i];
        }
        ed.lines[ed.cursor_y].len = ed.cursor_x;
        ed.line_count++;
        ed.cursor_y++;
        ed.cursor_x = 0;
        ed.dirty = true;
      }
    } else if (k == '\b' || k == 127) {
      if (ed.cursor_x > 0) {
        for (int i = ed.cursor_x - 1; i < ed.lines[ed.cursor_y].len - 1; i++) {
          ed.lines[ed.cursor_y].data[i] = ed.lines[ed.cursor_y].data[i + 1];
        }
        ed.lines[ed.cursor_y].len--;
        ed.cursor_x--;
        ed.dirty = true;
      } else if (ed.cursor_y > 0) {
        int prev = ed.cursor_y - 1;
        int old_len = ed.lines[prev].len;
        int cur_len = ed.lines[ed.cursor_y].len;
        if (old_len + cur_len < MAX_LINE_LEN) {
          for (int i = 0; i < cur_len; i++) {
            ed.lines[prev].data[old_len + i] = ed.lines[ed.cursor_y].data[i];
          }
          ed.lines[prev].len += cur_len;
          for (int i = ed.cursor_y; i < ed.line_count - 1; i++) {
            ed.lines[i] = ed.lines[i + 1];
          }
          ed.line_count--;
          ed.cursor_y--;
          ed.cursor_x = old_len;
          ed.dirty = true;
        }
      }
    } else if (k >= 32 && k < 127) {
      if (ed.lines[ed.cursor_y].len < MAX_LINE_LEN - 1) {
        for (int i = ed.lines[ed.cursor_y].len; i > ed.cursor_x; i--) {
          ed.lines[ed.cursor_y].data[i] = ed.lines[ed.cursor_y].data[i - 1];
        }
        ed.lines[ed.cursor_y].data[ed.cursor_x] = (char)k;
        ed.lines[ed.cursor_y].len++;
        ed.cursor_x++;
        ed.dirty = true;
      }
    }

    if (ed.cursor_x > ed.lines[ed.cursor_y].len)
      ed.cursor_x = ed.lines[ed.cursor_y].len;
    if (ed.cursor_y < ed.scroll_y)
      ed.scroll_y = ed.cursor_y;
    if (ed.cursor_y >= ed.scroll_y + (ed.term_h - STATUS_HEIGHT))
      ed.scroll_y = ed.cursor_y - (ed.term_h - STATUS_HEIGHT) + 1;
  }

  print(ANSI_CLEAR);
  exit(0);
  return 0;
}
