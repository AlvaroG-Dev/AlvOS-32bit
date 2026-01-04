#include "http.h"
#include "dns.h"
#include "irq.h"
#include "kernel.h"
#include "string.h"
#include "tcp.h"
#include "terminal.h"
#include "vfs.h"

static bool parse_url(const char *url, char *host, char *path) {
  if (strncmp(url, "http://", 7) != 0)
    return false;
  const char *start = url + 7;
  const char *slash = strchr(start, '/');
  if (slash) {
    int host_len = slash - start;
    if (host_len > 255)
      return false;
    strncpy(host, start, host_len);
    host[host_len] = '\0';
    strcpy(path, slash);
  } else {
    strcpy(host, start);
    strcpy(path, "/");
  }
  return true;
}

bool http_download(const char *url, const char *dest_path) {
  char host[256];
  char path[256];
  if (!parse_url(url, host, path)) {
    terminal_puts(&main_terminal, "[HTTP] Invalid URL\r\n");
    return false;
  }

  ip_addr_t server_ip;
  if (!dns_resolve(host, server_ip)) {
    terminal_puts(&main_terminal, "[HTTP] DNS failed\r\n");
    return false;
  }

  int socket = tcp_connect(server_ip, 80);
  if (socket < 0)
    return false;

  char request[1024];
  snprintf(request, sizeof(request),
           "GET %s HTTP/1.0\r\n"
           "Host: %s\r\n"
           "User-Agent: AlvOS/1.0\r\n"
           "Connection: close\r\n"
           "\r\n",
           path, host);

  tcp_send(socket, (uint8_t *)request, strlen(request));
  int fd = vfs_open(dest_path, VFS_O_WRONLY | VFS_O_CREAT);
  if (fd < 0) {
    tcp_close(socket);
    return false;
  }

  terminal_puts(&main_terminal, "[HTTP] Receiving...\r\n");

  uint8_t buffer[2048];
  bool header_parsed = false;
  int total_bytes = 0;
  int state = 0; // State machine for \r\n\r\n

  while (1) {
    int received = tcp_receive(socket, buffer, sizeof(buffer));
    if (received == -2)
      break; // Connection closed
    if (received <= 0)
      continue; // Timeout or error, retry

    for (int i = 0; i < received; i++) {
      if (!header_parsed) {
        // Buscando \r\n\r\n
        char c = (char)buffer[i];
        if (state == 0 && c == '\r')
          state = 1;
        else if (state == 1 && c == '\n')
          state = 2;
        else if (state == 2 && c == '\r')
          state = 3;
        else if (state == 3 && c == '\n') {
          state = 4;
          header_parsed = true;
          terminal_puts(&main_terminal,
                        "[HTTP] Headers OK, downloading...\r\n");
        } else {
          state = (c == '\r') ? 1 : 0;
        }
      } else {
        // Descargando cuerpo
        vfs_write(fd, &buffer[i], 1);
        total_bytes++;
        if (total_bytes % 4096 == 0)
          terminal_printf(&main_terminal, ".");
      }
    }
  }

  vfs_close(fd);
  tcp_close(socket);
  terminal_printf(&main_terminal, "\r\n[HTTP] Done. %d bytes -> %s\r\n",
                  total_bytes, dest_path);
  return true;
}
