#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_RESULTS 50
#define IPC_SOCKET "/tmp/yt_tui_mpv.sock"

typedef struct {
  char *title;
  char *url;
  int duration;
} Item;

typedef struct {
  Item items[MAX_RESULTS];
  int count;
  int selected;
  int scroll_offset;
  int playing_index;
  bool paused;
  char query[256];
} AppState;

static pid_t mpv_pid = -1;

static void free_items(AppState *st) {
  for (int i = 0; i < st->count; i++) {
    free(st->items[i].title);
    free(st->items[i].url);
    st->items[i].title = NULL;
    st->items[i].url = NULL;
  }
  st->count = 0;
  st->selected = 0;
  st->scroll_offset = 0;
  st->playing_index = -1;
}

static bool file_exists(const char *path) {
  struct stat sb;
  return stat(path, &sb) == 0;
}

static void mpv_ipc_send_raw(const char *msg) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, IPC_SOCKET, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
    ssize_t w1 = write(fd, msg, strlen(msg));
    ssize_t w2 = write(fd, "\n", 1);
    (void)w1; (void)w2;
  }
  close(fd);
}

static void mpv_toggle_pause(void) {
  mpv_ipc_send_raw("{\"command\":[\"cycle\",\"pause\"]}");
}

static void mpv_stop_playback(void) {
  mpv_ipc_send_raw("{\"command\":[\"stop\"]}");
}

static void mpv_load_url(const char *url) {
  char *escaped = NULL;
  size_t n = 0;
  FILE *mem = open_memstream(&escaped, &n);
  if (!mem) return;

  fputc('"', mem);
  for (const char *p = url; *p; p++) {
    if (*p == '"' || *p == '\\') fputc('\\', mem);
    fputc(*p, mem);
  }
  fputc('"', mem);
  fclose(mem);

  char cmd[4096];
  snprintf(cmd, sizeof(cmd),
           "{\"command\":[\"loadfile\",%s,\"replace\"]}", escaped);
  free(escaped);

  mpv_ipc_send_raw(cmd);
}

static void mpv_start_if_needed(void) {
  if (file_exists(IPC_SOCKET)) return;

  unlink(IPC_SOCKET);

  pid_t pid = fork();
  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
    execlp("mpv", "mpv",
           "--no-video",
           "--idle=yes",
           "--force-window=no",
           "--really-quiet",
           "--input-ipc-server=" IPC_SOCKET,
           (char *)NULL);
    _exit(127);
  }
  if (pid > 0) {
    mpv_pid = pid;
    for (int i = 0; i < 100; i++) {
      if (file_exists(IPC_SOCKET)) break;
      usleep(50 * 1000);
    }
  }
}

static void mpv_stop(void) {
  if (file_exists(IPC_SOCKET)) {
    mpv_ipc_send_raw("{\"command\":[\"quit\"]}");
    usleep(100 * 1000);
  }
  if (mpv_pid > 0) {
    kill(mpv_pid, SIGTERM);
    waitpid(mpv_pid, NULL, WNOHANG);
    mpv_pid = -1;
  }
  unlink(IPC_SOCKET);
}

static char *trim_whitespace(char *s) {
  if (!s) return s;
  while (*s && isspace((unsigned char)*s)) s++;
  if (*s == 0) return s;
  char *end = s + strlen(s) - 1;
  while (end > s && isspace((unsigned char)*end)) end--;
  end[1] = '\0';
  return s;
}

static int run_search(AppState *st, const char *raw_query) {
  free_items(st);

  char query_buf[256];
  strncpy(query_buf, raw_query, sizeof(query_buf) - 1);
  query_buf[sizeof(query_buf) - 1] = '\0';
  char *query = trim_whitespace(query_buf);
  
  if (!query[0]) return 0;

  // Escape per shell: sostituisci caratteri problematici
  // La query va dentro double quotes, quindi escape: " \ $ `
  char escaped_query[512];
  size_t j = 0;
  for (size_t i = 0; query[i] && j < sizeof(escaped_query) - 5; i++) {
    char c = query[i];
    if (c == '"' || c == '\\' || c == '$' || c == '`') {
      escaped_query[j++] = '\\';
    }
    escaped_query[j++] = c;
  }
  escaped_query[j] = '\0';

  // Formato: "ytsearchN:query" tutto insieme in double quotes
  // Usiamo ||| come separatore - non richiede escape e non appare nei titoli
  char cmd[2048];
  snprintf(cmd, sizeof(cmd),
           "yt-dlp --flat-playlist "
           "--print '%%(title)s|||%%(id)s' "
           "\"ytsearch%d:%s\" 2>&1",
           MAX_RESULTS, escaped_query);
  
  // DEBUG: scrivi comando su file
  FILE *dbg = fopen("/tmp/yt-tui-debug.log", "w");
  if (dbg) {
    fprintf(dbg, "Command: %s\n", cmd);
    fclose(dbg);
  }

  FILE *fp = popen(cmd, "r");
  if (!fp) return -1;

  // DEBUG: log output
  dbg = fopen("/tmp/yt-tui-debug.log", "a");
  if (dbg) fprintf(dbg, "Output:\n");

  char *line = NULL;
  size_t cap = 0;
  int count = 0;

  while (count < MAX_RESULTS && getline(&line, &cap, fp) != -1) {
    // DEBUG
    if (dbg) fprintf(dbg, "LINE: %s", line);
    
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
      line[--len] = '\0';
    }
    
    if (!line[0]) continue;
    
    // Salta linee di errore
    if (strncmp(line, "ERROR", 5) == 0) continue;
    if (strncmp(line, "WARNING", 7) == 0) continue;

    char *sep = strstr(line, "|||");
    
    // DEBUG: mostra se separatore trovato e i primi bytes
    if (dbg) {
      fprintf(dbg, "  -> sep found: %s, first 5 bytes hex: ", sep ? "YES" : "NO");
      for (int k = 0; k < 5 && line[k]; k++) {
        fprintf(dbg, "%02x ", (unsigned char)line[k]);
      }
      fprintf(dbg, "\n");
    }
    
    if (!sep) continue;
    *sep = '\0';
    
    const char *title = line;
    const char *video_id = sep + 3;  // skip "|||"

    if (!video_id[0]) continue;
    // Verifica ID valido (11 caratteri alfanumerici + - _ )
    size_t id_len = strlen(video_id);
    if (id_len < 5 || id_len > 20) continue;

    char fullurl[256];
    snprintf(fullurl, sizeof(fullurl), 
             "https://www.youtube.com/watch?v=%s", video_id);

    st->items[count].title = strdup(title);
    st->items[count].url = strdup(fullurl);
    st->items[count].duration = 0;
    
    if (st->items[count].title && st->items[count].url) {
      count++;
    } else {
      free(st->items[count].title);
      free(st->items[count].url);
      st->items[count].title = NULL;
      st->items[count].url = NULL;
    }
  }

  free(line);
  pclose(fp);
  
  // DEBUG
  if (dbg) {
    fprintf(dbg, "Total count: %d\n", count);
    fclose(dbg);
  }

  st->count = count;
  st->selected = 0;
  st->scroll_offset = 0;
  st->playing_index = -1;
  st->paused = false;

  strncpy(st->query, query, sizeof(st->query) - 1);
  st->query[sizeof(st->query) - 1] = '\0';

  return count;
}

static void format_duration(int sec, char out[16]) {
  if (sec <= 0) {
    snprintf(out, 16, "--:--");
    return;
  }
  int h = sec / 3600;
  int m = (sec % 3600) / 60;
  int s = sec % 60;
  if (h > 0) {
    snprintf(out, 16, "%d:%02d:%02d", h, m, s);
  } else {
    snprintf(out, 16, "%02d:%02d", m, s);
  }
}

static void draw_ui(const AppState *st, const char *status) {
  erase();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  attron(A_BOLD);
  mvprintw(0, 0, " ShellBeats v-0.1 ");
  attroff(A_BOLD);
  printw(" |  /: cerca  |  Enter: play  |  Spazio: pausa  |  n/p: next/prev  |  q: esci");
  
  mvhline(1, 0, ACS_HLINE, cols);

  mvprintw(2, 0, "Query: ");
  attron(A_BOLD);
  printw("%s", st->query[0] ? st->query : "(nessuna)");
  attroff(A_BOLD);
  
  mvprintw(2, cols - 20, "Risultati: %d", st->count);

  if (status && status[0]) {
    mvprintw(3, 0, ">>> %s", status);
  }

  mvhline(4, 0, ACS_HLINE, cols);

  int list_top = 5;
  int list_height = rows - list_top - 2;
  if (list_height < 1) list_height = 1;

  int start = st->scroll_offset;
  if (st->selected < start) {
    start = st->selected;
  } else if (st->selected >= start + list_height) {
    start = st->selected - list_height + 1;
  }

  for (int i = 0; i < list_height && (start + i) < st->count; i++) {
    int idx = start + i;
    bool is_selected = (idx == st->selected);
    bool is_playing = (idx == st->playing_index);

    int y = list_top + i;
    move(y, 0);
    clrtoeol();

    char mark = ' ';
    if (is_playing) {
      mark = st->paused ? '|' : '>';
      attron(A_BOLD);
    }

    if (is_selected) {
      attron(A_REVERSE);
    }

    char dur[16];
    format_duration(st->items[idx].duration, dur);

    int max_title = cols - 14;
    if (max_title < 20) max_title = 20;

    char titlebuf[1024];
    const char *title = st->items[idx].title ? st->items[idx].title : "(no title)";
    strncpy(titlebuf, title, sizeof(titlebuf) - 1);
    titlebuf[sizeof(titlebuf) - 1] = '\0';

    if ((int)strlen(titlebuf) > max_title) {
      if (max_title > 3) {
        titlebuf[max_title - 3] = '.';
        titlebuf[max_title - 2] = '.';
        titlebuf[max_title - 1] = '.';
        titlebuf[max_title] = '\0';
      }
    }

    mvprintw(y, 0, " %c %3d. [%s] %s", mark, idx + 1, dur, titlebuf);

    if (is_selected) {
      attroff(A_REVERSE);
    }
    if (is_playing) {
      attroff(A_BOLD);
    }
  }

  mvhline(rows - 2, 0, ACS_HLINE, cols);
  
  if (st->playing_index >= 0 && st->items[st->playing_index].title) {
    mvprintw(rows - 1, 0, " Now playing: ");
    attron(A_BOLD);
    
    int max_np = cols - 20;
    char npbuf[512];
    strncpy(npbuf, st->items[st->playing_index].title, sizeof(npbuf) - 1);
    npbuf[sizeof(npbuf) - 1] = '\0';
    if ((int)strlen(npbuf) > max_np && max_np > 3) {
      npbuf[max_np - 3] = '.';
      npbuf[max_np - 2] = '.';
      npbuf[max_np - 1] = '.';
      npbuf[max_np] = '\0';
    }
    printw("%s", npbuf);
    attroff(A_BOLD);
    
    if (st->paused) {
      printw(" [PAUSA]");
    }
  }

  refresh();
}

// Input manuale - gestisce correttamente caratteri senza timeout
static int get_string_input(char *buf, size_t bufsz, const char *prompt) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  
  int y = rows - 1;
  move(y, 0);
  clrtoeol();
  
  attron(A_BOLD);
  mvprintw(y, 0, "%s", prompt);
  attroff(A_BOLD);
  refresh();
  
  int prompt_len = strlen(prompt);
  int max_input = cols - prompt_len - 2;
  if (max_input > (int)bufsz - 1) max_input = bufsz - 1;
  if (max_input < 1) max_input = 1;
  
  echo();
  curs_set(1);
  move(y, prompt_len);
  
  // Usa getnstr che è blocking
  getnstr(buf, max_input);
  
  noecho();
  curs_set(0);
  
  // Trim
  char *trimmed = trim_whitespace(buf);
  if (trimmed != buf) {
    memmove(buf, trimmed, strlen(trimmed) + 1);
  }
  
  return strlen(buf);
}

static void play_index(AppState *st, int idx) {
  if (idx < 0 || idx >= st->count) return;
  if (!st->items[idx].url) return;
  
  mpv_start_if_needed();
  mpv_load_url(st->items[idx].url);
  st->playing_index = idx;
  st->paused = false;
}

static void show_help(void) {
  erase();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  (void)cols;

  int y = 2;
  attron(A_BOLD);
  mvprintw(y++, 2, "ShellBeats v-0.1 | Aiuto");
  attroff(A_BOLD);
  y++;
  
  mvprintw(y++, 4, "/           Cerca su YouTube");
  mvprintw(y++, 4, "Enter       Riproduci il brano selezionato");
  mvprintw(y++, 4, "Spazio      Pausa/Riprendi");
  mvprintw(y++, 4, "n           Prossimo brano");
  mvprintw(y++, 4, "p           Brano precedente");
  mvprintw(y++, 4, "Su/Giu/j/k  Naviga nella lista");
  mvprintw(y++, 4, "PgUp/PgDn   Pagina su/giu");
  mvprintw(y++, 4, "g/G         Inizio/Fine lista");
  mvprintw(y++, 4, "x           Ferma riproduzione");
  mvprintw(y++, 4, "h o ?       Mostra questo aiuto");
  mvprintw(y++, 4, "q           Esci");
  y++;
  
  mvprintw(y++, 4, "Requisiti: yt-dlp, mpv");
  y++;
  
  attron(A_REVERSE);
  mvprintw(rows - 2, 2, " Premi un tasto per continuare... ");
  attroff(A_REVERSE);
  
  refresh();
  getch();
}

static bool check_dependencies(char *errmsg, size_t errsz) {
  FILE *fp = popen("which yt-dlp 2>/dev/null", "r");
  if (fp) {
    char buf[256];
    bool found = (fgets(buf, sizeof(buf), fp) != NULL && buf[0] == '/');
    pclose(fp);
    if (!found) {
      snprintf(errmsg, errsz, "yt-dlp non trovato! Installa con: pip install yt-dlp");
      return false;
    }
  }
  
  fp = popen("which mpv 2>/dev/null", "r");
  if (fp) {
    char buf[256];
    bool found = (fgets(buf, sizeof(buf), fp) != NULL && buf[0] == '/');
    pclose(fp);
    if (!found) {
      snprintf(errmsg, errsz, "mpv non trovato! Installa con: apt install mpv");
      return false;
    }
  }
  
  return true;
}

int main(void) {
  setlocale(LC_ALL, "");
  
  AppState st = {0};
  st.selected = 0;
  st.scroll_offset = 0;
  st.playing_index = -1;
  st.paused = false;

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);

  char status[512] = "";
  
  if (!check_dependencies(status, sizeof(status))) {
    draw_ui(&st, status);
    getch();
    endwin();
    fprintf(stderr, "%s\n", status);
    return 1;
  }
  
  snprintf(status, sizeof(status), "Premi / per cercare, h per aiuto.");
  draw_ui(&st, status);

  bool running = true;
  while (running) {
    int ch = getch();
    
    if (ch == ERR) {
      continue;
    }

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;
    int list_height = rows - 7;
    if (list_height < 1) list_height = 1;

    switch (ch) {
      case 'q':
      case 'Q':
        running = false;
        break;

      case KEY_UP:
      case 'k':
        if (st.selected > 0) {
          st.selected--;
          if (st.selected < st.scroll_offset) {
            st.scroll_offset = st.selected;
          }
        }
        break;

      case KEY_DOWN:
      case 'j':
        if (st.selected + 1 < st.count) {
          st.selected++;
          if (st.selected >= st.scroll_offset + list_height) {
            st.scroll_offset = st.selected - list_height + 1;
          }
        }
        break;

      case KEY_PPAGE:
        st.selected -= list_height;
        if (st.selected < 0) st.selected = 0;
        st.scroll_offset = st.selected;
        break;

      case KEY_NPAGE:
        st.selected += list_height;
        if (st.selected >= st.count) st.selected = st.count - 1;
        if (st.selected < 0) st.selected = 0;
        if (st.selected >= st.scroll_offset + list_height) {
          st.scroll_offset = st.selected - list_height + 1;
        }
        break;

      case KEY_HOME:
      case 'g':
        st.selected = 0;
        st.scroll_offset = 0;
        break;

      case KEY_END:
      case 'G':
        if (st.count > 0) {
          st.selected = st.count - 1;
          st.scroll_offset = st.selected - list_height + 1;
          if (st.scroll_offset < 0) st.scroll_offset = 0;
        }
        break;

      case '\n':
      case KEY_ENTER:
        if (st.count > 0) {
          play_index(&st, st.selected);
          snprintf(status, sizeof(status), "Riproduco: %s", 
                   st.items[st.selected].title ? st.items[st.selected].title : "?");
        }
        break;

      case ' ':
        if (st.playing_index >= 0 && file_exists(IPC_SOCKET)) {
          mpv_toggle_pause();
          st.paused = !st.paused;
          snprintf(status, sizeof(status), st.paused ? "In pausa" : "Riproduzione");
        }
        break;

      case 'x':
      case 'X':
        if (st.playing_index >= 0) {
          mpv_stop_playback();
          st.playing_index = -1;
          st.paused = false;
          snprintf(status, sizeof(status), "Riproduzione fermata");
        }
        break;

      case 'n':
      case 'N':
        if (st.count > 0) {
          int next = (st.playing_index >= 0) ? st.playing_index + 1 : st.selected;
          if (next >= st.count) next = 0;
          st.selected = next;
          play_index(&st, next);
          snprintf(status, sizeof(status), "Prossimo: %s",
                   st.items[next].title ? st.items[next].title : "?");
        }
        break;

      case 'p':
      case 'P':
        if (st.count > 0) {
          int prev = (st.playing_index >= 0) ? st.playing_index - 1 : st.selected;
          if (prev < 0) prev = st.count - 1;
          st.selected = prev;
          play_index(&st, prev);
          snprintf(status, sizeof(status), "Precedente: %s",
                   st.items[prev].title ? st.items[prev].title : "?");
        }
        break;

      case '/':
      case 's':
      case 'S':
        {
          char q[256] = {0};
          int len = get_string_input(q, sizeof(q), "Cerca: ");
          if (len > 0) {
            snprintf(status, sizeof(status), "Ricerca: %s ...", q);
            draw_ui(&st, status);
            
            int r = run_search(&st, q);
            if (r < 0) {
              snprintf(status, sizeof(status), "Errore durante la ricerca!");
            } else if (r == 0) {
              snprintf(status, sizeof(status), "Nessun risultato per: %s", q);
            } else {
              snprintf(status, sizeof(status), "Trovati %d risultati per: %s", r, q);
            }
          } else {
            snprintf(status, sizeof(status), "Ricerca annullata");
          }
        }
        break;

      case 'h':
      case 'H':
      case '?':
        show_help();
        break;

      case KEY_RESIZE:
        clear();
        break;
    }

    draw_ui(&st, status);
  }

  endwin();
  free_items(&st);
  mpv_stop();
  
  return 0;
}
