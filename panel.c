#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQ (1024 * 1024)
#define DOCKER_SOCKET "/var/run/docker.sock"
#define CONFIG_PATH "/data/panel.conf"

typedef struct {
  char user[128];
  char password[256];
  char protected[4096];
} Config;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} Buf;

static void die(const char *msg) {
  perror(msg);
  exit(1);
}

static void buf_init(Buf *b) {
  b->cap = 8192;
  b->len = 0;
  b->data = malloc(b->cap);
  if (!b->data) die("malloc");
  b->data[0] = 0;
}

static void buf_need(Buf *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap) return;
  while (b->len + extra + 1 > b->cap) b->cap *= 2;
  b->data = realloc(b->data, b->cap);
  if (!b->data) die("realloc");
}

static void buf_append(Buf *b, const char *s, size_t n) {
  buf_need(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
}

static void buf_puts(Buf *b, const char *s) {
  buf_append(b, s, strlen(s));
}

static void buf_printf(Buf *b, const char *fmt, ...) {
  va_list ap;
  char small[1024];
  va_start(ap, fmt);
  int n = vsnprintf(small, sizeof(small), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(small)) {
    buf_append(b, small, (size_t)n);
    return;
  }
  char *tmp = malloc((size_t)n + 1);
  if (!tmp) die("malloc");
  va_start(ap, fmt);
  vsnprintf(tmp, (size_t)n + 1, fmt, ap);
  va_end(ap);
  buf_append(b, tmp, (size_t)n);
  free(tmp);
}

static int contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = hay; *p; p++) {
    if (strncasecmp(p, needle, n) == 0) return 1;
  }
  return 0;
}

static int write_all(int fd, const char *body, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, body + sent, len - sent);
    if (n <= 0) return 0;
    sent += (size_t)n;
  }
  return 1;
}

static char *read_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  Buf b;
  buf_init(&b);
  char tmp[8192];
  size_t n = 0;
  while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
    buf_append(&b, tmp, n);
  }
  fclose(f);
  if (len_out) *len_out = b.len;
  return b.data;
}

static void trim_inplace(char *s) {
  if (!s) return;
  char *start = s;
  while (isspace((unsigned char)*start)) start++;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

static void config_init(Config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  const char *user = getenv("PANEL_USER");
  const char *password = getenv("PANEL_PASSWORD");
  snprintf(cfg->user, sizeof(cfg->user), "%s", user && *user ? user : "admin");
  snprintf(cfg->password, sizeof(cfg->password), "%s", password && *password ? password : "123456");
  snprintf(cfg->protected, sizeof(cfg->protected), "%s", "docker-panel");
}

static void config_load(Config *cfg) {
  config_init(cfg);
  size_t len = 0;
  char *text = read_file(CONFIG_PATH, &len);
  if (!text) return;
  char *saveptr = NULL;
  char *line = strtok_r(text, "\n", &saveptr);
  while (line) {
    trim_inplace(line);
    if (strncmp(line, "USER=", 5) == 0) {
      snprintf(cfg->user, sizeof(cfg->user), "%s", line + 5);
    } else if (strncmp(line, "PASSWORD=", 9) == 0) {
      snprintf(cfg->password, sizeof(cfg->password), "%s", line + 9);
    } else if (strncmp(line, "PROTECTED=", 10) == 0) {
      snprintf(cfg->protected, sizeof(cfg->protected), "%s", line + 10);
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }
  trim_inplace(cfg->user);
  trim_inplace(cfg->password);
  trim_inplace(cfg->protected);
  if (!*cfg->user) snprintf(cfg->user, sizeof(cfg->user), "%s", "admin");
  if (!*cfg->password) snprintf(cfg->password, sizeof(cfg->password), "%s", "123456");
  if (!*cfg->protected) snprintf(cfg->protected, sizeof(cfg->protected), "%s", "docker-panel");
  free(text);
}

static int config_save(Config *cfg) {
  FILE *f = fopen(CONFIG_PATH ".tmp", "wb");
  if (!f) return 0;
  fprintf(f, "USER=%s\n", cfg->user);
  fprintf(f, "PASSWORD=%s\n", cfg->password);
  fprintf(f, "PROTECTED=%s\n", cfg->protected);
  fclose(f);
  return rename(CONFIG_PATH ".tmp", CONFIG_PATH) == 0;
}

static int protected_contains(Config *cfg, const char *name) {
  if (!name || !*name) return 0;
  char copy[sizeof(cfg->protected)];
  snprintf(copy, sizeof(copy), "%s", cfg->protected);
  char *saveptr = NULL;
  char *item = strtok_r(copy, ",", &saveptr);
  while (item) {
    trim_inplace(item);
    if (strcmp(item, name) == 0) return 1;
    item = strtok_r(NULL, ",", &saveptr);
  }
  return 0;
}

static void protected_add(Config *cfg, const char *name) {
  if (!name || !*name || protected_contains(cfg, name)) return;
  if (!*cfg->protected) {
    snprintf(cfg->protected, sizeof(cfg->protected), "%s", name);
  } else if (strlen(cfg->protected) + strlen(name) + 2 < sizeof(cfg->protected)) {
    strncat(cfg->protected, ",", sizeof(cfg->protected) - strlen(cfg->protected) - 1);
    strncat(cfg->protected, name, sizeof(cfg->protected) - strlen(cfg->protected) - 1);
  }
}

static void protected_remove(Config *cfg, const char *name) {
  if (!name || !*name) return;
  char copy[sizeof(cfg->protected)];
  char next[sizeof(cfg->protected)] = {0};
  snprintf(copy, sizeof(copy), "%s", cfg->protected);
  char *saveptr = NULL;
  char *item = strtok_r(copy, ",", &saveptr);
  while (item) {
    trim_inplace(item);
    if (*item && strcmp(item, name) != 0) {
      if (*next) strncat(next, ",", sizeof(next) - strlen(next) - 1);
      strncat(next, item, sizeof(next) - strlen(next) - 1);
    }
    item = strtok_r(NULL, ",", &saveptr);
  }
  snprintf(cfg->protected, sizeof(cfg->protected), "%s", next);
}

static void send_raw(int fd, int status, const char *type, const char *body, size_t len, int auth) {
  const char *msg = "OK";
  if (status == 400) msg = "Bad Request";
  if (status == 401) msg = "Unauthorized";
  if (status == 403) msg = "Forbidden";
  if (status == 404) msg = "Not Found";
  if (status == 500) msg = "Internal Server Error";
  dprintf(fd, "HTTP/1.1 %d %s\r\n", status, msg);
  dprintf(fd, "Content-Type: %s\r\n", type);
  dprintf(fd, "Content-Length: %zu\r\n", len);
  dprintf(fd, "Cache-Control: no-store\r\n");
  if (auth) dprintf(fd, "WWW-Authenticate: Basic realm=\"Docker Panel\"\r\n");
  dprintf(fd, "Connection: close\r\n\r\n");
  if (len) write_all(fd, body, len);
}

static void send_text(int fd, int status, const char *text) {
  send_raw(fd, status, "text/plain; charset=utf-8", text, strlen(text), 0);
}

static void send_json_text(int fd, int status, const char *text) {
  send_raw(fd, status, "application/json; charset=utf-8", text, strlen(text), 0);
}

static int b64val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static char *base64_decode(const char *in) {
  size_t len = strlen(in);
  char *out = malloc(len * 3 / 4 + 4);
  if (!out) die("malloc");
  int val = 0, valb = -8;
  size_t pos = 0;
  for (size_t i = 0; i < len; i++) {
    if (in[i] == '=') break;
    int d = b64val((unsigned char)in[i]);
    if (d < 0) continue;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out[pos++] = (char)((val >> valb) & 0xff);
      valb -= 8;
    }
  }
  out[pos] = 0;
  return out;
}

static char *header_value(const char *headers, const char *name) {
  size_t nlen = strlen(name);
  const char *p = headers;
  while (p && *p) {
    const char *line_end = strstr(p, "\r\n");
    size_t llen = line_end ? (size_t)(line_end - p) : strlen(p);
    if (llen > nlen + 1 && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
      const char *v = p + nlen + 1;
      while (*v == ' ' || *v == '\t') v++;
      size_t vlen = llen - (size_t)(v - p);
      char *out = malloc(vlen + 1);
      if (!out) die("malloc");
      memcpy(out, v, vlen);
      out[vlen] = 0;
      return out;
    }
    if (!line_end) break;
    p = line_end + 2;
  }
  return NULL;
}

static int is_authed(const char *headers) {
  Config cfg;
  config_load(&cfg);
  const char *user = cfg.user;
  const char *pass = cfg.password;
  char *auth = header_value(headers, "Authorization");
  if (!auth) return 0;
  int ok = 0;
  if (strncasecmp(auth, "Basic ", 6) == 0) {
    char *decoded = base64_decode(auth + 6);
    size_t need = strlen(user) + strlen(pass) + 2;
    char *expected = malloc(need);
    if (!expected) die("malloc");
    snprintf(expected, need, "%s:%s", user, pass);
    ok = strcmp(decoded, expected) == 0;
    free(expected);
    free(decoded);
  }
  free(auth);
  return ok;
}

static void json_escape(Buf *b, const char *s) {
  buf_append(b, "\"", 1);
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      char tmp[2] = {'\\', (char)c};
      buf_append(b, tmp, 2);
    } else if (c == '\n') {
      buf_puts(b, "\\n");
    } else if (c == '\r') {
      buf_puts(b, "\\r");
    } else if (c == '\t') {
      buf_puts(b, "\\t");
    } else if (c < 32) {
      buf_printf(b, "\\u%04x", c);
    } else {
      buf_append(b, (const char *)&c, 1);
    }
  }
  buf_append(b, "\"", 1);
}

static void config_protected_json(Buf *b, Config *cfg) {
  buf_puts(b, "[");
  char copy[sizeof(cfg->protected)];
  snprintf(copy, sizeof(copy), "%s", cfg->protected);
  char *saveptr = NULL;
  char *item = strtok_r(copy, ",", &saveptr);
  int first = 1;
  while (item) {
    trim_inplace(item);
    if (*item) {
      if (!first) buf_puts(b, ",");
      json_escape(b, item);
      first = 0;
    }
    item = strtok_r(NULL, ",", &saveptr);
  }
  buf_puts(b, "]");
}

static char *url_encode(const char *s) {
  Buf b;
  buf_init(&b);
  const char *hex = "0123456789ABCDEF";
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      buf_append(&b, (const char *)&c, 1);
    } else {
      char tmp[3] = {'%', hex[c >> 4], hex[c & 15]};
      buf_append(&b, tmp, 3);
    }
  }
  return b.data;
}

static char *json_get_string(const char *json, const char *key) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  char *p = strstr((char *)json, pat);
  if (!p) return NULL;
  p = strchr(p + strlen(pat), ':');
  if (!p) return NULL;
  p++;
  while (isspace((unsigned char)*p)) p++;
  if (*p != '"') return NULL;
  p++;
  Buf b;
  buf_init(&b);
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'n') buf_append(&b, "\n", 1);
      else if (*p == 'r') buf_append(&b, "\r", 1);
      else if (*p == 't') buf_append(&b, "\t", 1);
      else buf_append(&b, p, 1);
      p++;
    } else {
      buf_append(&b, p, 1);
      p++;
    }
  }
  return b.data;
}

static long long json_get_int(const char *json, const char *key) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  char *p = strstr((char *)json, pat);
  if (!p) return 0;
  p = strchr(p + strlen(pat), ':');
  if (!p) return 0;
  p++;
  while (isspace((unsigned char)*p)) p++;
  return atoll(p);
}

static char *decode_chunked(const char *body, size_t len, size_t *out_len) {
  Buf out;
  buf_init(&out);
  size_t pos = 0;
  while (pos < len) {
    char *end = memmem(body + pos, len - pos, "\r\n", 2);
    if (!end) break;
    char sizebuf[32];
    size_t slen = (size_t)(end - (body + pos));
    if (slen >= sizeof(sizebuf)) break;
    memcpy(sizebuf, body + pos, slen);
    sizebuf[slen] = 0;
    long chunk = strtol(sizebuf, NULL, 16);
    pos += slen + 2;
    if (chunk <= 0) break;
    if (pos + (size_t)chunk > len) break;
    buf_append(&out, body + pos, (size_t)chunk);
    pos += (size_t)chunk + 2;
  }
  *out_len = out.len;
  return out.data;
}

static int docker_request(const char *method, const char *path, const char *body, char **out) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 599;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, DOCKER_SOCKET, sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return 598;
  }

  size_t body_len = body ? strlen(body) : 0;
  dprintf(fd, "%s %s HTTP/1.1\r\nHost: docker\r\nConnection: close\r\n", method, path);
  if (body) {
    dprintf(fd, "Content-Type: application/json\r\nContent-Length: %zu\r\n", body_len);
  }
  dprintf(fd, "\r\n");
  if (body_len) write_all(fd, body, body_len);

  Buf resp;
  buf_init(&resp);
  char tmp[8192];
  ssize_t n;
  while ((n = read(fd, tmp, sizeof(tmp))) > 0) buf_append(&resp, tmp, (size_t)n);
  close(fd);

  int status = 500;
  sscanf(resp.data, "HTTP/%*s %d", &status);
  char *header_end = strstr(resp.data, "\r\n\r\n");
  if (!header_end) {
    *out = resp.data;
    return status;
  }
  size_t header_len = (size_t)(header_end + 4 - resp.data);
  size_t raw_body_len = resp.len - header_len;
  char *headers = strndup(resp.data, header_len);
  char *decoded = NULL;
  size_t decoded_len = 0;
  if (headers && contains_ci(headers, "Transfer-Encoding: chunked")) {
    decoded = decode_chunked(resp.data + header_len, raw_body_len, &decoded_len);
  } else {
    decoded = malloc(raw_body_len + 1);
    if (!decoded) die("malloc");
    memcpy(decoded, resp.data + header_len, raw_body_len);
    decoded[raw_body_len] = 0;
    decoded_len = raw_body_len;
  }
  if (headers) free(headers);
  free(resp.data);
  decoded[decoded_len] = 0;
  *out = decoded;
  return status;
}

static char *container_name_from_id(const char *id) {
  if (!id || !*id) return NULL;
  char *eid = url_encode(id);
  char req[512];
  snprintf(req, sizeof(req), "/containers/%s/json", eid);
  free(eid);
  char *body = NULL;
  int status = docker_request("GET", req, NULL, &body);
  if (status < 200 || status >= 300 || !body) {
    if (body) free(body);
    return NULL;
  }
  char *name = json_get_string(body, "Name");
  free(body);
  if (name && name[0] == '/') memmove(name, name + 1, strlen(name));
  return name;
}

static int container_is_protected(Config *cfg, const char *id) {
  if (!id || !*id) return 0;
  if (protected_contains(cfg, id)) return 1;
  char short_id[13] = {0};
  snprintf(short_id, sizeof(short_id), "%.12s", id);
  if (protected_contains(cfg, short_id)) return 1;
  char *name = container_name_from_id(id);
  int protected = name && protected_contains(cfg, name);
  if (name) free(name);
  return protected;
}

static void read_mem(long long *total, long long *available) {
  *total = 0;
  *available = 0;
  size_t len = 0;
  char *text = read_file("/host/proc/meminfo", &len);
  if (!text) text = read_file("/proc/meminfo", &len);
  if (!text) return;
  char *line = strtok(text, "\n");
  while (line) {
    long long v = 0;
    if (sscanf(line, "MemTotal: %lld kB", &v) == 1) *total = v * 1024;
    if (sscanf(line, "MemAvailable: %lld kB", &v) == 1) *available = v * 1024;
    line = strtok(NULL, "\n");
  }
  free(text);
}

static int read_cpu(long long *idle, long long *total) {
  size_t len = 0;
  char *text = read_file("/host/proc/stat", &len);
  if (!text) text = read_file("/proc/stat", &len);
  if (!text) return 0;
  long long vals[10] = {0};
  int got = sscanf(text, "cpu %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
                   &vals[0], &vals[1], &vals[2], &vals[3], &vals[4],
                   &vals[5], &vals[6], &vals[7], &vals[8], &vals[9]);
  free(text);
  if (got < 4) return 0;
  *idle = vals[3] + vals[4];
  *total = 0;
  for (int i = 0; i < got; i++) *total += vals[i];
  return 1;
}

static double host_uptime(void) {
  size_t len = 0;
  char *text = read_file("/host/proc/uptime", &len);
  if (!text) text = read_file("/proc/uptime", &len);
  if (!text) return 0;
  double up = atof(text);
  free(text);
  return up;
}

static char *overview_json(void) {
  Config cfg;
  config_load(&cfg);
  char *containers = NULL, *images = NULL, *info = NULL;
  int sc = docker_request("GET", "/containers/json?all=1", NULL, &containers);
  int si = docker_request("GET", "/images/json?all=1", NULL, &images);
  int sf = docker_request("GET", "/info", NULL, &info);
  if (sc < 200 || sc >= 300) { free(containers); containers = strdup("[]"); }
  if (si < 200 || si >= 300) { free(images); images = strdup("[]"); }
  if (sf < 200 || sf >= 300) { free(info); info = strdup("{}"); }

  long long mem_total = 0, mem_avail = 0;
  read_mem(&mem_total, &mem_avail);
  long long mem_used = mem_total > mem_avail ? mem_total - mem_avail : 0;
  double mem_pct = mem_total ? ((double)mem_used / (double)mem_total) * 100.0 : 0.0;

  long long idle1 = 0, total1 = 0, idle2 = 0, total2 = 0;
  double cpu_pct = 0.0;
  if (read_cpu(&idle1, &total1)) {
    usleep(180000);
    if (read_cpu(&idle2, &total2) && total2 > total1) {
      long long didle = idle2 - idle1;
      long long dtotal = total2 - total1;
      cpu_pct = (1.0 - ((double)didle / (double)dtotal)) * 100.0;
      if (cpu_pct < 0) cpu_pct = 0;
      if (cpu_pct > 100) cpu_pct = 100;
    }
  }

  struct statvfs vfs;
  unsigned long long disk_size = 0, disk_avail = 0, disk_used = 0;
  if (statvfs("/hostfs", &vfs) != 0) statvfs("/", &vfs);
  disk_size = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
  disk_avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
  disk_used = disk_size > disk_avail ? disk_size - disk_avail : 0;
  double disk_pct = disk_size ? ((double)disk_used / (double)disk_size) * 100.0 : 0.0;

  char host[256] = "unknown";
  gethostname(host, sizeof(host) - 1);
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char iso[64];
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  char *server_version = json_get_string(info, "ServerVersion");
  if (!server_version) server_version = strdup("");

  Buf out;
  buf_init(&out);
  buf_printf(&out, "{\"generatedAt\":\"%s\",", iso);
  buf_puts(&out, "\"host\":{");
  buf_printf(&out, "\"name\":");
  json_escape(&out, host);
  buf_printf(&out, ",\"platform\":\"linux\",\"uptime\":%.0f,", host_uptime());
  buf_printf(&out, "\"cpu\":{\"percent\":%.1f,\"cores\":%ld},", cpu_pct, sysconf(_SC_NPROCESSORS_ONLN));
  buf_printf(&out, "\"memory\":{\"total\":%lld,\"available\":%lld,\"used\":%lld,\"percent\":%.1f},",
             mem_total, mem_avail, mem_used, mem_pct);
  buf_printf(&out, "\"storage\":{\"size\":%llu,\"used\":%llu,\"available\":%llu,\"usePercent\":\"%.0f%%\"}",
             disk_size, disk_used, disk_avail, disk_pct);
  buf_puts(&out, "},");
  buf_printf(&out, "\"docker\":{\"containers\":%lld,\"running\":%lld,\"paused\":%lld,\"stopped\":%lld,\"images\":%lld,\"serverVersion\":",
             json_get_int(info, "Containers"), json_get_int(info, "ContainersRunning"),
             json_get_int(info, "ContainersPaused"), json_get_int(info, "ContainersStopped"),
             json_get_int(info, "Images"));
  json_escape(&out, server_version);
  buf_puts(&out, "},\"containers\":");
  buf_puts(&out, containers);
  buf_puts(&out, ",\"images\":");
  buf_puts(&out, images);
  buf_puts(&out, ",\"config\":{\"user\":");
  json_escape(&out, cfg.user);
  buf_puts(&out, ",\"protected\":");
  config_protected_json(&out, &cfg);
  buf_puts(&out, "}");
  buf_puts(&out, "}");

  free(containers);
  free(images);
  free(info);
  free(server_version);
  return out.data;
}

static char *json_payload_create(const char *image, const char *command, const char *ports, const char *restart) {
  Buf b;
  buf_init(&b);
  buf_puts(&b, "{\"Image\":");
  json_escape(&b, image);
  if (command && *command) {
    buf_puts(&b, ",\"Cmd\":[\"sh\",\"-lc\",");
    json_escape(&b, command);
    buf_puts(&b, "]");
  }
  char host_port[32] = {0}, cont_port[32] = {0};
  if (ports && sscanf(ports, "%31[^:]:%31s", host_port, cont_port) == 2) {
    char *slash = strchr(cont_port, '/');
    if (!slash) strncat(cont_port, "/tcp", sizeof(cont_port) - strlen(cont_port) - 1);
    buf_puts(&b, ",\"ExposedPorts\":{");
    json_escape(&b, cont_port);
    buf_puts(&b, ":{}}");
    buf_puts(&b, ",\"HostConfig\":{\"PortBindings\":{");
    json_escape(&b, cont_port);
    buf_puts(&b, ":[{\"HostPort\":");
    json_escape(&b, host_port);
    buf_puts(&b, "}]},\"RestartPolicy\":{\"Name\":");
    json_escape(&b, restart && *restart ? restart : "unless-stopped");
    buf_puts(&b, "}}");
  } else {
    buf_puts(&b, ",\"HostConfig\":{\"RestartPolicy\":{\"Name\":");
    json_escape(&b, restart && *restart ? restart : "unless-stopped");
    buf_puts(&b, "}}");
  }
  buf_puts(&b, "}");
  return b.data;
}

static void api_error(int fd, const char *message) {
  Buf b;
  buf_init(&b);
  buf_puts(&b, "{\"error\":");
  json_escape(&b, message);
  buf_puts(&b, "}");
  send_json_text(fd, 400, b.data);
  free(b.data);
}

static void handle_api(int fd, const char *method, const char *path, const char *body) {
  if (strcmp(method, "GET") == 0 && strcmp(path, "/api/overview") == 0) {
    char *json = overview_json();
    send_json_text(fd, 200, json);
    free(json);
    return;
  }

  Config cfg;
  config_load(&cfg);
  char *docker_body = NULL;
  int status = 404;
  if (strcmp(method, "POST") == 0 && strcmp(path, "/api/container/start") == 0) {
    char *id = json_get_string(body, "id");
    if (!id) return api_error(fd, "container id is required");
    if (container_is_protected(&cfg, id)) {
      free(id);
      return api_error(fd, "该容器已受保护，不能执行控制操作");
    }
    char *eid = url_encode(id);
    char req[512];
    snprintf(req, sizeof(req), "/containers/%s/start", eid);
    status = docker_request("POST", req, NULL, &docker_body);
    free(id); free(eid);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/container/stop") == 0) {
    char *id = json_get_string(body, "id");
    if (!id) return api_error(fd, "container id is required");
    if (container_is_protected(&cfg, id)) {
      free(id);
      return api_error(fd, "该容器已受保护，不能执行控制操作");
    }
    char *eid = url_encode(id);
    char req[512];
    snprintf(req, sizeof(req), "/containers/%s/stop?t=10", eid);
    status = docker_request("POST", req, NULL, &docker_body);
    free(id); free(eid);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/container/delete") == 0) {
    char *id = json_get_string(body, "id");
    if (!id) return api_error(fd, "container id is required");
    if (container_is_protected(&cfg, id)) {
      free(id);
      return api_error(fd, "该容器已受保护，不能执行控制操作");
    }
    char *eid = url_encode(id);
    char req[512];
    snprintf(req, sizeof(req), "/containers/%s?force=1&v=1", eid);
    status = docker_request("DELETE", req, NULL, &docker_body);
    free(id); free(eid);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/container/create") == 0) {
    char *image = json_get_string(body, "image");
    char *name = json_get_string(body, "name");
    char *command = json_get_string(body, "command");
    char *ports = json_get_string(body, "ports");
    char *restart = json_get_string(body, "restart");
    if (!image || !*image) return api_error(fd, "image is required");
    char *payload = json_payload_create(image, command, ports, restart);
    char req[512];
    if (name && *name) {
      char *ename = url_encode(name);
      snprintf(req, sizeof(req), "/containers/create?name=%s", ename);
      free(ename);
    } else {
      snprintf(req, sizeof(req), "/containers/create");
    }
    status = docker_request("POST", req, payload, &docker_body);
    free(payload); free(image); if (name) free(name); if (command) free(command); if (ports) free(ports); if (restart) free(restart);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/image/pull") == 0) {
    char *image = json_get_string(body, "image");
    if (!image || !*image) return api_error(fd, "image is required");
    char *eimage = url_encode(image);
    char req[1024];
    snprintf(req, sizeof(req), "/images/create?fromImage=%s", eimage);
    status = docker_request("POST", req, NULL, &docker_body);
    free(image); free(eimage);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/image/delete") == 0) {
    char *image = json_get_string(body, "image");
    if (!image || !*image) return api_error(fd, "image is required");
    char *eimage = url_encode(image);
    char req[1024];
    snprintf(req, sizeof(req), "/images/%s?force=1", eimage);
    status = docker_request("DELETE", req, NULL, &docker_body);
    free(image); free(eimage);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/protection/add") == 0) {
    char *name = json_get_string(body, "name");
    char *id = json_get_string(body, "id");
    if ((!name || !*name) && id && *id) name = container_name_from_id(id);
    if (!name || !*name) return api_error(fd, "protected container name is required");
    trim_inplace(name);
    if (name[0] == '/') memmove(name, name + 1, strlen(name));
    protected_add(&cfg, name);
    if (!config_save(&cfg)) {
      if (name) free(name);
      if (id) free(id);
      return api_error(fd, "保存保护配置失败");
    }
    if (name) free(name);
    if (id) free(id);
    send_json_text(fd, 200, "{\"ok\":true}");
    return;
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/protection/remove") == 0) {
    char *name = json_get_string(body, "name");
    if (!name || !*name) return api_error(fd, "protected container name is required");
    trim_inplace(name);
    if (name[0] == '/') memmove(name, name + 1, strlen(name));
    protected_remove(&cfg, name);
    if (!config_save(&cfg)) {
      free(name);
      return api_error(fd, "保存保护配置失败");
    }
    free(name);
    send_json_text(fd, 200, "{\"ok\":true}");
    return;
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/account/update") == 0) {
    char *user = json_get_string(body, "user");
    char *password = json_get_string(body, "password");
    if (!user || !*user || !password || !*password) {
      if (user) free(user);
      if (password) free(password);
      return api_error(fd, "账号和密码不能为空");
    }
    trim_inplace(user);
    trim_inplace(password);
    if (!*user || !*password) {
      free(user);
      free(password);
      return api_error(fd, "账号和密码不能为空");
    }
    snprintf(cfg.user, sizeof(cfg.user), "%s", user);
    snprintf(cfg.password, sizeof(cfg.password), "%s", password);
    free(user);
    free(password);
    if (!config_save(&cfg)) return api_error(fd, "保存账号密码失败");
    send_json_text(fd, 200, "{\"ok\":true}");
    return;
  } else {
    send_json_text(fd, 404, "{\"error\":\"not found\"}");
    return;
  }

  if (status >= 200 && status < 300) {
    send_json_text(fd, 200, "{\"ok\":true}");
  } else {
    api_error(fd, docker_body && *docker_body ? docker_body : "docker api request failed");
  }
  if (docker_body) free(docker_body);
}

static void serve_static(int fd, const char *path) {
  if (strstr(path, "..")) {
    send_text(fd, 403, "Forbidden");
    return;
  }
  char file[1024];
  if (strcmp(path, "/") == 0) snprintf(file, sizeof(file), "/app/public/index.html");
  else {
    if (strlen(path) > 480) {
      send_text(fd, 404, "Not found");
      return;
    }
    snprintf(file, sizeof(file), "/app/public/%s", path + 1);
  }
  size_t len = 0;
  char *data = read_file(file, &len);
  if (!data) {
    send_text(fd, 404, "Not found");
    return;
  }
  const char *type = strstr(file, ".js") ? "application/javascript; charset=utf-8" : "text/html; charset=utf-8";
  send_raw(fd, 200, type, data, len, 0);
  free(data);
}

static void handle_client(int fd) {
  Buf req;
  buf_init(&req);
  char tmp[4096];
  char *header_end = NULL;
  while (req.len < MAX_REQ) {
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n <= 0) break;
    buf_append(&req, tmp, (size_t)n);
    header_end = strstr(req.data, "\r\n\r\n");
    if (header_end) break;
  }
  if (!header_end) {
    send_text(fd, 400, "Bad request");
    free(req.data);
    return;
  }
  size_t header_len = (size_t)(header_end + 4 - req.data);
  char *headers = strndup(req.data, header_len);
  char method[16] = {0}, url[512] = {0};
  sscanf(req.data, "%15s %511s", method, url);
  char *cl = header_value(headers, "Content-Length");
  size_t content_len = cl ? (size_t)strtoull(cl, NULL, 10) : 0;
  if (cl) free(cl);
  while (req.len < header_len + content_len && req.len < MAX_REQ) {
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n <= 0) break;
    buf_append(&req, tmp, (size_t)n);
  }
  char *body = req.data + header_len;

  if (!is_authed(headers)) {
    send_raw(fd, 401, "text/plain; charset=utf-8", "Authentication required", 23, 1);
  } else if (strncmp(url, "/api/", 5) == 0) {
    handle_api(fd, method, url, body);
  } else {
    serve_static(fd, url);
  }

  free(headers);
  free(req.data);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  const char *port_env = getenv("PORT");
  int port = port_env ? atoi(port_env) : 8080;
  if (port <= 0) port = 8080;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) die("socket");
  int yes = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
  if (listen(srv, 64) < 0) die("listen");
  fprintf(stderr, "docker panel listening on %d\n", port);

  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) continue;
    handle_client(fd);
    close(fd);
  }
}
