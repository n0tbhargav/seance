// seancectl: command-line client for a running seance's control socket.
//   seancectl list
//   seancectl read [PANE] [scrollback]
//   seancectl send [PANE] TEXT...        paste-style insert; C escapes (\n \r \t \e \xHH \\); no Enter
//   seancectl exec [PANE] COMMAND...     type COMMAND verbatim (no escape processing), then press Enter
//   seancectl key  [PANE] KEY...         enter esc tab up down ... ctrl-c alt-x shift-tab f5 a..z A..Z 0..9 and punctuation (: ; % ...)
//   seancectl wait [PANE] REGEX [SECS]   poll the screen until REGEX matches (exit 0) or SECS elapse (exit 1, default 30)
//   seancectl focus|close PANE   seancectl split PANE right|down|left|up   seancectl newtab [PANE]
//   seancectl events                     stream events (command_finished, bell, notification, title, focus, pane_*)
// PANE is a numeric id or "focused"; defaults to $SEANCE_PANE if set, else the focused pane.
// Socket: -s PATH, else $SEANCE_SOCKET, else the newest $XDG_RUNTIME_DIR/seance-*.sock.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static char sockpath[sizeof(((struct sockaddr_un *)0)->sun_path)];

static int find_socket(void) {
  if (sockpath[0]) return 0;
  const char *env = getenv("SEANCE_SOCKET");
  if (env && *env) { snprintf(sockpath, sizeof sockpath, "%s", env); return 0; }
  // Same directory the host uses: g_get_user_runtime_dir() = $XDG_RUNTIME_DIR, else ~/.cache
  char dir[512];
  const char *rt = getenv("XDG_RUNTIME_DIR");
  if (rt && *rt) snprintf(dir, sizeof dir, "%s", rt);
  else {
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (cache && *cache) snprintf(dir, sizeof dir, "%s", cache);
    else snprintf(dir, sizeof dir, "%s/.cache", home ? home : "/tmp");
  }
  DIR *d = opendir(dir);
  if (!d) return -1;
  struct dirent *e;
  time_t best = 0;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "seance-", 7) || !strstr(e->d_name, ".sock")) continue;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_mtime >= best) { best = st.st_mtime; snprintf(sockpath, sizeof sockpath, "%s", path); }
  }
  closedir(d);
  return sockpath[0] ? 0 : -1;
}

static int connect_sock(void) {
  if (find_socket() < 0) { fprintf(stderr, "seancectl: no seance socket found (set SEANCE_SOCKET or use -s)\n"); return -1; }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { fprintf(stderr, "seancectl: cannot connect to %s: %s\n", sockpath, strerror(errno)); close(fd); return -1; }
  return fd;
}

static ssize_t read_line(int fd, char *buf, size_t max) {
  size_t n = 0;
  while (n + 1 < max) {
    char c;
    ssize_t r = read(fd, &c, 1);
    if (r <= 0) return n ? (ssize_t)n : r;
    if (c == '\n') break;
    buf[n++] = c;
  }
  buf[n] = 0;
  return (ssize_t)n;
}

// Send one command; returns malloc'd payload (NUL-terminated) or NULL on error. *len = payload bytes.
static char *request(int fd, const char *line, size_t *len) {
  size_t ll = strlen(line);
  if (write(fd, line, ll) < 0 || write(fd, "\n", 1) < 0) return NULL;
  char hdr[512];
  if (read_line(fd, hdr, sizeof hdr) <= 0) { fprintf(stderr, "seancectl: connection closed\n"); return NULL; }
  if (strncmp(hdr, "OK ", 3)) { fprintf(stderr, "seancectl: %s\n", strncmp(hdr, "ERR ", 4) ? hdr : hdr + 4); return NULL; }
  size_t n = strtoul(hdr + 3, NULL, 10);
  char *buf = malloc(n + 1);
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, buf + got, n - got);
    if (r <= 0) { free(buf); return NULL; }
    got += (size_t)r;
  }
  buf[n] = 0;
  if (len) *len = n;
  return buf;
}

static int is_number(const char *s) {
  if (!s || !*s) return 0;
  for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
  return 1;
}

int main(int argc, char **argv) {
  int i = 1;
  if (i + 1 < argc && !strcmp(argv[i], "-s")) { snprintf(sockpath, sizeof sockpath, "%s", argv[i + 1]); i += 2; }
  if (i >= argc) { fprintf(stderr, "usage: seancectl [-s SOCK] list|read|send|exec|key|wait|focus|split|newtab|close|events ...\n"); return 2; }
  const char *cmd = argv[i++];
  const char *envpane = getenv("SEANCE_PANE");
  char line[65536];

  int fd = connect_sock();
  if (fd < 0) return 1;

  if (!strcmp(cmd, "list")) snprintf(line, sizeof line, "LIST");
  else if (!strcmp(cmd, "newtab")) snprintf(line, sizeof line, "NEWTAB %s", i < argc ? argv[i] : "");
  else if (!strcmp(cmd, "events")) {
    char *r = request(fd, "SUBSCRIBE", NULL);
    if (!r) return 1;
    free(r);
    char l[4096];
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (read_line(fd, l, sizeof l) > 0) puts(l);
    return 0;
  } else {
    // commands with a pane argument (defaults to $SEANCE_PANE)
    const char *pane = envpane ? envpane : "focused";   // $SEANCE_PANE, else whichever pane has focus
    if (i < argc && (is_number(argv[i]) || !strcmp(argv[i], "focused"))) pane = argv[i++];
    char rest[60000] = "";
    for (int k = i; k < argc; k++) { if (k > i) strncat(rest, " ", sizeof rest - strlen(rest) - 1); strncat(rest, argv[k], sizeof rest - strlen(rest) - 1); }

    if (!strcmp(cmd, "wait")) {
      if (i >= argc) { fprintf(stderr, "usage: seancectl wait [PANE] REGEX [SECS]\n"); return 2; }
      const char *pat = argv[i];
      double secs = i + 1 < argc ? atof(argv[i + 1]) : 30;
      regex_t re;
      if (regcomp(&re, pat, REG_EXTENDED | REG_NEWLINE | REG_NOSUB)) { fprintf(stderr, "seancectl: bad regex\n"); return 2; }
      struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
      for (;;) {
        snprintf(line, sizeof line, "READ %s", pane);
        char *screen = request(fd, line, NULL);
        if (!screen) return 1;
        int m = regexec(&re, screen, 0, NULL, 0) == 0;
        free(screen);
        if (m) return 0;
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 > secs) { fprintf(stderr, "seancectl: timed out waiting for /%s/\n", pat); return 1; }
        usleep(200000);
      }
    }
    const char *proto = !strcmp(cmd, "read") ? "READ" : !strcmp(cmd, "send") ? "SEND" : !strcmp(cmd, "exec") ? "EXEC"
                        : !strcmp(cmd, "key") ? "KEY" : !strcmp(cmd, "focus") ? "FOCUS" : !strcmp(cmd, "split") ? "SPLIT"
                        : !strcmp(cmd, "close") ? "CLOSE" : NULL;
    if (!proto) { fprintf(stderr, "seancectl: unknown command '%s'\n", cmd); return 2; }
    snprintf(line, sizeof line, "%s %s %s", proto, pane, rest);
  }

  size_t n = 0;
  char *out = request(fd, line, &n);
  if (!out) return 1;
  fwrite(out, 1, n, stdout);
  free(out);
  return 0;
}
