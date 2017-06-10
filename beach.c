#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdarg.h>

typedef int16_t i16;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#if defined(SERVER)
#define serverMain main
#elif defined(CLIENT)
#define clientMain main
#endif

char *configfile = "./config";
char *savefile = "./data";
char *tempfile = "./.temp";
char *logFile = "./log";
FILE *logStream;
pthread_mutex_t logMutex;

#define MAX_CONN 3

typedef struct Connection {
  int id;
  int csd;
  int prev;
  int next;
} Connection;

typedef struct ConnectionList {
  Connection *conn;
  u32 count;
  u32 max;
  int closed;
  int open;
  pthread_mutex_t mutex;
} ConnectionList;

typedef struct String {
  char *str;
  size_t size;
  size_t len;
} String;

typedef struct Booking {
  i16 start, end;
  u32 user;
} Booking;

typedef struct BookingList {
  Booking *booking;
  u32 count;
  u32 capacity;
  u32 lockUser;
  i16 lockDay;
  pthread_mutex_t mutex;
} BookingList;

typedef struct Season {
  int nRows, nCols;
  int nUmbrella;
  int year;
  int start, end;
  BookingList *bookingList;
} Season;

Season *season;
ConnectionList *conns;

//prints formatted text to the logfile
void mprintf(const char *format, ...);

//cancats fomatted text to a String and grows it if it doesn't fit
int dcatf(String *string, const char *format, ...);

//convert year, month, day to an integer (1-365)
int getYday(int currentYear, int year, int mon, int mday);
//returns yday for the current day
int getCurrentYday();
//convert date formatted like dd/mm/yyyy into yday
int parseDate(char *str, int currentYear);
//convert date formatted like dd/mm/yyyy into the corresponding year
int parseYear(char *str);
//convert yday into a string formatted like dd/mm/yyyy
int getDateString(char *out, size_t len, int year, int yday);

void loadConfig(Season *season);
void initBookingList(Season *season);
void saveBookingList(Season *season);
void loadBookingList(Season *season);

//lock for preventing multiple users to book at the same time
int lockBooking(Season *season, u32 user, u32 idUmbrella);
void unlockBooking(Season *season, u32 idUmbrella);

int removeBooking(Season *season, u32 user, u32 idUmbrella);
int addBooking(Season *season, u32 user, u32 idUmbrella, i16 start, i16 end);
int testBooking(Season *season, u32 user, u32 idUmbrella, i16 start, i16 end);

//write text to a socket
int swrite(int socket, char *text);
//read from a socket and split into tokens
int readToks(int csd, char *buffer, size_t bufferSize, char *toks[], int maxToks);

//thread for communication with a single user
void *socketListener(void *commSocket);
//thread for accepting users connections
void *socketAccept(void *masterSocket);

void initConnectionList(ConnectionList *conns);
Connection *addConnection(ConnectionList *conns, int csd);
void removeConnection(ConnectionList *conns, int id);
void closeConnections(ConnectionList *conns, Season *season);

//macro for detecting errors
#define CHECK(RESULT) if ((RESULT) == -1) _exitErrno(errno, #RESULT , __LINE__)
#define exitError(id, msg) _exitError(id, msg, __LINE__)
void _exitError(int id, char *msg, int lineNumber) {
  mprintf("error: %d, %s\nat line: %d\n", id, msg, lineNumber);
  exit(id);
}

void _exitErrno(int id, char *source, int lineNumber) {
  mprintf("line: %d - %s, error: %s\n", lineNumber, source, strerror(id));
  exit(id);
}

void term(int sig) {
  closeConnections(conns, season);
  saveBookingList(season);
  mprintf("exit!\n");
  exit(0);
}

void mprintf(const char *format, ...) {
  va_list args;
  va_start (args, format);
  pthread_mutex_lock(&logMutex);
  vfprintf(logStream, format, args);
  fflush(logStream);
  pthread_mutex_unlock(&logMutex);
  va_end(args);
}

int dcatf(String *string, const char *format, ...) {
  va_list args;
  va_start (args, format);
  char tmp[100];
  int len = vsprintf(tmp, format, args) + 1;
  int tlen = len + string->len;
  if (tlen > string->size) {
    string->size *= 2;
    if (string->size < tlen) string->size = tlen;
    string->str = realloc(string->str, string->size);
  }
  memcpy(string->str + string->len, tmp, len);
  va_end(args);
  string->len = --tlen;
  return tlen;
}

int isLeap(int year) {
  return (!(year % 4) && (year % 100)) || !(year % 400);
}
const int pdays[] = {
  0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365,
  0, 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};
// returns -1 if the date is not valid
int getYday(int currentYear, int year, int mon, int mday) {
  if (currentYear != -1 && currentYear != year ) return -1;
  if (mon < 1 || mon > 12) return -1;
  if (mday < 1 || mday > (pdays[mon+1] - pdays[mon])) return -1;
  return pdays[mon + 14 * isLeap(year)] + mday - 1;
}

int getCurrentYday() {
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  return getYday(-1, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

// returns - 1 if the date is not valid
int parseDate(char *str, int currentYear) {
  int year, mon, mday;
  int ntok = sscanf(str, "%d/%d/%d", &mday, &mon, &year);
  if (ntok != 3) return -1;
  return getYday(currentYear, year, mon, mday);
}

int parseYear(char *str) {
  int year = 0;
  sscanf(str, "%*d/%*d/%d", &year);
  return year;
}

//requires the buffer to be 11 chars long
//returns -1 if the yday is not valid
int getDateString(char *out, size_t len, int year, int yday) {
  int leap = isLeap(year);
  int max = leap ? 366 : 365;
  if (yday >= max) return -1;
  int mon;
  for (mon = 1; yday > pdays[mon+1]; mon++);
  int mday = yday - pdays[mon] + 1;
  snprintf(out, len, "%02d/%02d/%04d", mday, mon, year);
  return 0;
}

void loadConfig(Season *season) {
  FILE *fp = fopen(configfile, "r");
  if (fp == NULL) exitError(-1, "config file not found");

  char *tokstate;
  char *line = NULL;
  size_t bufSize = 0;
  int count = 1;
  while (getline(&line, &bufSize, fp) != -1) {
    char *key = strtok_r(line, " =", &tokstate);
    char *value = strtok_r(NULL, " =", &tokstate);
    if (key == NULL) continue;
    if (key[0] == '#') continue;
    if (value == NULL) exitError( -1, "invalid config file");

    if (!strcmp(key, "start")) {
      int yday = parseDate(value, -1);
      int year = parseYear(value);
      season->start = yday;
      season->year = year;
    } else if (!strcmp(key, "end")) {
      int yday = parseDate(value, -1);
      season->end = yday;
    } else if (!strcmp(key, "rows")) {
      int nRows = atoi(value);
      season->nRows = nRows;
    } else if (!strcmp(key, "cols")) {
      int nCols = atoi(value);
      season->nCols = nCols;
    } else {
      exitError( -1, "invalid config file");
    }

    count++;
  }
  if (season->nCols == 0 || season->nRows == 0 ||
      season->start == -1 || season->end == -1 ||
      season->year == 0)
    exitError( -1, "invalid config file");

  season->nUmbrella = season->nCols * season->nRows;
  fclose(fp);
}

void initBookingList(Season *season) {
  season->bookingList = calloc(season->nUmbrella, sizeof(BookingList));
  for (u32 i = 0; i < season->nUmbrella; i++) {
    pthread_mutex_init(&(season->bookingList[i].mutex), NULL);
  }
}

void saveBookingList(Season *season) {
  FILE *fp = fopen(tempfile, "w");
  if (fp == NULL) {
    mprintf("failed to open file for writing.\n");
    return;
  }

  for (u32 i = 0; i < season->nUmbrella; i++) {
    BookingList *list = season->bookingList + i;
    pthread_mutex_lock(&list->mutex);
    Booking *array = list->booking;
    u32 count = list->count;
    fprintf(fp, "%d %d %d", count, list->lockUser, list->lockDay);
    for (u32 j = 0; j < count; j++) {
      Booking *booking = array + j;
      fprintf(fp, " %d %d %d", booking->user, booking->start, booking->end);
    }
    fprintf(fp, "\n");
    pthread_mutex_unlock(&list->mutex);
  }
  fclose(fp);

  remove(savefile);
  rename(tempfile, savefile);
}

void loadBookingList(Season *season) {
  FILE *fp;
  fp = fopen(savefile, "r");
  if (fp == NULL) {
    if (errno == ENOENT) {
      mprintf("Il file 'data' non esiste, creazione di un nuovo database.\n");
    } else {
      exitError(errno, "Impossibile aprire il file 'data'.");
    }
  }

  char *line = NULL;
  size_t bufSize = 0;
  for (u32 i = 0; i < season->nUmbrella; i++) {
    if (getline(&line, &bufSize, fp) == -1) exitError(i, "this line is missing.");

    BookingList *list = season->bookingList + i;
    pthread_mutex_lock(&list->mutex);

    char *tokstate;
    char *countToken = strtok_r(line, " ", &tokstate);
    char *userToken = strtok_r(NULL, " ", &tokstate);
    char *dayToken = strtok_r(NULL, " ", &tokstate);
    if (dayToken == NULL) exitError(i, "error on parsing this line of file.");
    u32 count = atoi(countToken);
    list->booking = realloc(list->booking, count * sizeof(Booking));
    list->count = count;
    list->capacity = count;
    list->lockUser = atoi(userToken);
    list->lockDay = atoi(dayToken);

    for (u32 j = 0; j < count; j++) {
      char *user = strtok_r(NULL, " ", &tokstate);
      char *start = strtok_r(NULL, " ", &tokstate);
      char *end = strtok_r(NULL, " ", &tokstate);
      if (end == NULL) exitError(i, "too few arguments on this line.");
      Booking *booking = list->booking + j;
      booking->user = atoi(user);
      booking->start = atoi(start);
      booking->end = atoi(end);
      if (booking->start > booking->end) exitError(i, "invalid booking.");
    }
    if (strtok_r(NULL, " ", &tokstate) != NULL) exitError(i, " too many arguments on this line.");
    pthread_mutex_unlock(&list->mutex);
  }
  fclose(fp);
  mprintf("Database caricato in memoria.\n");
}

void unlockBooking(Season *season, u32 idUmbrella) {
  if (idUmbrella >= season->nUmbrella) return;
  BookingList *list = season->bookingList + idUmbrella;
  pthread_mutex_lock(&list->mutex);
  list->lockUser = 0;
  pthread_mutex_unlock(&list->mutex);
}

int lockBooking(Season *season, u32 user, u32 idUmbrella) {
  if (idUmbrella >= season->nUmbrella) return 0;
  BookingList *list = season->bookingList + idUmbrella;
  pthread_mutex_lock(&list->mutex);
  int today = getCurrentYday();
  int avail = (list->lockUser == 0 || list->lockUser == user || list->lockDay < today);
  if (avail) {
    list->lockUser = user;
    list->lockDay = today;
  }
  pthread_mutex_unlock(&list->mutex);
  return avail;
}

int removeBooking(Season *season, u32 user, u32 idUmbrella) {
  if (idUmbrella >= season->nUmbrella) return -1;

  BookingList *list = season->bookingList + idUmbrella;
  pthread_mutex_lock(&list->mutex);
  Booking *array = list->booking;

  for (int i = list->count-1; i >= 0; i--) {
    if (array[i].user == user) {
      list->count--;
      if (i <= list->count)
        memmove(array + i, array + i + 1, (list->count - i) * sizeof(Booking));
    }
  }

  pthread_mutex_unlock(&list->mutex);
  return 0;
}

int _testSetBooking(Season *season, u32 user, u32 idUmbrella, i16 start, i16 end, int testOnly) {
  if (idUmbrella >= season->nUmbrella) return -1;
  if (start > end)                     return -1;
  if (start < season->start)           return -1;
  if (end > season->end)               return -1;

  BookingList *list = season->bookingList + idUmbrella;
  pthread_mutex_lock(&list->mutex);
  Booking *array = list->booking;

  u32 i;
  for (i = 0; i < list->count; i++) {
    if (array[i].end >= start) {
      if (array[i].start > end) break;
      else goto fail;
    }
  }
  if(testOnly) goto success;
  if (list->count == list->capacity) {
    list->capacity += list->capacity + 1;
    array = list->booking = realloc(list->booking, sizeof(Booking) * list->capacity);
  }
  if (i < list->count)
    memmove(array + i + 1, array + i, (list->count - i) * sizeof(Booking));
  array[i].user = user;
  array[i].start = start;
  array[i].end = end;
  list->count++;
  list->lockUser = 0;
success:
  pthread_mutex_unlock(&list->mutex);
  return 0;
fail:
  pthread_mutex_unlock(&list->mutex);
  return -1;
}

//returns 0 if the booking is available
int testBooking(Season *season, u32 user, u32 idUmbrella, i16 start, i16 end) {
  return _testSetBooking(season, user, idUmbrella, start, end, 1);
}
//returns 0 if the booking is successful
int addBooking(Season *season, u32 user, u32 idUmbrella, i16 start, i16 end) {
  return _testSetBooking(season, user, idUmbrella, start, end, 0);
}

int swrite(int socket, char *text) {
  int size = strlen(text) + 1;
  return write(socket, text, size);
}

int readToks(int csd, char *buffer, size_t bufferSize, char *toks[], int maxToks) {
  static const char sep[] = " \n\r";
  int error = read(csd, buffer, bufferSize);
  if (error < 1) return error;
  if (error >= bufferSize) error = bufferSize -1;
  buffer[error] = 0;
  char *tokstate;
  int nToks = 0;
  toks[0] = strtok_r(buffer, sep, &tokstate);
  while(toks[nToks]) {
    nToks++;
    if (nToks >= maxToks) break;
    toks[nToks] = strtok_r(NULL, sep, &tokstate);
  }
  return nToks;
}

int ckm(char *a, char *b, int n1, int n2) {
  return !strcmp(a, b) && n1 == n2;
}

void *socketListener(void *connection) {
  Connection *conn = (Connection *)connection;
  int csd = conn->csd;
  int logged = 0;
  int user = 0;
  struct timeval tv = {0};
  tv.tv_sec = 60;
  setsockopt(csd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(struct timeval));

  swrite(csd, "welcome");

  char buf[100];
  char *toks[10];
  int nToks;
  for (;;) {
    if ((nToks = readToks(csd, buf, 100, toks, 10)) < 1) break;
    if (!logged) {
      if (ckm(toks[0], "login", nToks, 2)) {
        user = atoi(toks[1]);
        logged = 1;
        swrite(csd, "ok");
      } else swrite(csd, "nlogin");
    } else {

      if (ckm(toks[0], "book", nToks, 1)) {
        swrite(csd, "ok");

        if ((nToks = readToks(csd, buf, 100, toks, 10)) < 1) break;
        if (ckm(toks[0], "book", nToks, 2)) {
          int nUmbrella = atoi(toks[1]);
          if (lockBooking(season, user, nUmbrella)) { 

            swrite(csd, "available");
            if ((nToks = readToks(csd, buf, 100, toks, 10)) < 1) break;
            if (ckm(toks[0], "book", nToks, 3) ||
                ckm(toks[0], "book", nToks, 4)) {
              int start, end;
              if (nToks < 4) {
                start = getCurrentYday();
                end = parseDate(toks[2], season->year);
              } else {
                start = parseDate(toks[2], season->year);
                end = parseDate(toks[3], season->year);
              }
              if (!addBooking(season, user, nUmbrella, start, end)) { // se data disponibile
                swrite(csd, "done");
              } else {
                swrite(csd, "navailable");
                unlockBooking(season, nUmbrella);
              }

            } else if (ckm(toks[0], "cancel", nToks, 1)) {
              unlockBooking(season, nUmbrella);
              swrite(csd, "ok");

            } else swrite(csd, "failed");
          } else swrite(csd, "navailable");
        } else swrite(csd, "failed");

      } else if (ckm(toks[0], "available", nToks, 3) ||
                 ckm(toks[0], "available", nToks, 2) ||
                 ckm(toks[0], "available", nToks, 1)) {
        int start, end;
        if (nToks == 1) {
          start = end = getCurrentYday();
        } else if (nToks == 2) {
          start = getCurrentYday();
          end = parseDate(toks[1], season->year);
        } else {
          start = parseDate(toks[1], season->year);
          end = parseDate(toks[2], season->year);
        }
        String umb = {};
        dcatf(&umb, "available");
        for (int i = 0; i < season->nUmbrella; i++) {
          int avail = testBooking(season, user, i, start, end);
          if (!avail) dcatf(&umb, " %d", i);
        }
        if (!strcmp(umb.str, "available")) swrite(csd, "navailable");
        else swrite(csd, umb.str);
        free(umb.str);

      } else if (ckm(toks[0], "availrow", nToks, 4) ||
          ckm(toks[0], "availrow", nToks, 3) ||
          ckm(toks[0], "availrow", nToks, 2)) {
        int start, end;
        if (nToks == 2) {
          start = end = getCurrentYday();
        } else if (nToks == 3) {
          start = getCurrentYday();
          end = parseDate(toks[2], season->year);
        } else {
          start = parseDate(toks[2], season->year);
          end = parseDate(toks[3], season->year);
        }
        int row = atoi(toks[1]);
        int is = row * season->nCols;
        int ie = is + season->nCols;
        String umb = {};
        if (row < 0 || row >= season->nRows) swrite(csd, "navailable");
        else {
          dcatf(&umb, "available");
          for (int i = is; i < ie; i++) {
            int avail = testBooking(season, user, i, start, end);
            if (!avail) dcatf(&umb, " %d", i);
          }
          if (!strcmp(umb.str, "available")) swrite(csd, "navailable");
          else swrite(csd, umb.str);
        }
        free(umb.str);

      } else if (ckm(toks[0], "cancel", nToks, 2)) {
        int nUmbrella = atoi(toks[1]);
        if (!removeBooking(season, user, nUmbrella)) swrite(csd, "cancel ok");
        else swrite(csd, "failed");
      } else if (ckm(toks[0], "logout", nToks, 1)) {
        swrite(csd, "bye");
        break;
      } else if (ckm(toks[0], "save", nToks, 1)) {
        saveBookingList(season);
        swrite(csd, "ok");
      } else if (ckm(toks[0], "today", nToks, 1)) {
        char dateStr[32];
        getDateString(dateStr, 32, season->year, getCurrentYday());
        swrite(csd, dateStr);
      } else if (ckm(toks[0], "start", nToks, 1)) {
        char dateStr[32];
        getDateString(dateStr, 32, season->year, season->start);
        swrite(csd, dateStr);
      } else if (ckm(toks[0], "end", nToks, 1)) {
        char dateStr[32];
        getDateString(dateStr, 32, season->year, season->end);
        swrite(csd, dateStr);
      } else swrite(csd, "unknown");
    }
  }

  close(csd);
  removeConnection(conns, conn->id);
  return NULL;
}

void initConnectionList(ConnectionList *conns) {
  conns->count = 0;
  conns->max = MAX_CONN;
  conns->closed = 0;
  conns->open = -1;
  pthread_mutex_init(&conns->mutex, 0);
  Connection *conn = malloc(sizeof(Connection) * MAX_CONN);
  for (int i = 0; i < MAX_CONN; i++){
    conn[i].id = i;
    conn[i].next = i+1;
  }
  conn[MAX_CONN-1].next = -1;
  conns->conn = conn;
}

Connection *addConnection(ConnectionList *conns, int csd) {
  Connection *conn = NULL;
  pthread_mutex_lock(&conns->mutex);

  if (conns->closed != -1) {
    conn = conns->conn + conns->closed;
    conns->closed = conn->next;
    conn->csd = csd;
    conn->next = conns->open;
    conn->prev = -1;
    conns->open = conn->id;
    if (conn->next != -1) conns->conn[conn->next].prev = conn->id;
    conns->count++;
  }

  pthread_mutex_unlock(&conns->mutex);
  return conn;
}

void removeConnection(ConnectionList *conns, int id) {
  pthread_mutex_lock(&conns->mutex);

  Connection *conn = conns->conn + id;
  if (conn->next != -1) conns->conn[conn->next].prev = conn->prev;
  if (conn->prev != -1) conns->conn[conn->prev].next = conn->next;
  else  conns->open = conn->next;
  conn->next = conns->closed;
  conns->closed = conn->id;
  conns->count--;

  pthread_mutex_unlock(&conns->mutex);
}

void closeConnections(ConnectionList *conns, Season *season) {
  pthread_mutex_lock(&conns->mutex);
  for (int i = 0; i < season->nUmbrella; i++)
    pthread_mutex_lock(&(season->bookingList[i].mutex));

  //usleep(10000000);
  int next = conns->open;
  while (next != -1) {
    Connection *c = conns->conn + next;
    close(c->csd);
    conns->count--;
    next = c->next;
  }
  conns->closed = -1;

  for (int i = 0; i < season->nUmbrella; i++)
    pthread_mutex_unlock(&(season->bookingList[i].mutex));
  pthread_mutex_unlock(&conns->mutex);
}

int serverMain(int argc, char **argv) {
  signal(SIGINT, term);
  signal(SIGTERM, term);
  signal(SIGQUIT, term);
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(12345);
  sa.sin_addr.s_addr = INADDR_ANY;

  logStream = fopen(logFile, "w");
  pthread_mutex_init(&logMutex, NULL);

  season = calloc(1, sizeof(Season));
  loadConfig(season);
  initBookingList(season);
  loadBookingList(season);

  conns = malloc(sizeof(ConnectionList));
  initConnectionList(conns);

  int msd;
  CHECK(msd = socket(AF_INET, SOCK_STREAM, 0));
  CHECK(bind(msd, (struct sockaddr *)&sa, sizeof(sa)));
  listen(msd, 10); //buffer 10 requests

  for (;;) {
    int csd = accept(msd, NULL, 0);
    Connection *conn = addConnection(conns, csd);
    if (conn == NULL) {
      swrite(csd, "serverfull");
      close(csd);
      continue;
    }
    pthread_t child;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&child, &attr, socketListener, conn);
  }
  return 0;
}

int clientMain(int argc, char **argv) {
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(12345);
  sa.sin_addr.s_addr = inet_addr("127.0.0.1");

  int sd;
  CHECK(sd = socket(AF_INET, SOCK_STREAM, 0));
  CHECK(connect(sd, (struct sockaddr*)&sa, sizeof(sa)));

  char buffer[1000];
  char *line = NULL;
  size_t size = 0;
  int len = 0;
  while(1) {
    memset(buffer, 0, 1000);
    if (read(sd, buffer, 1000) < 1) break;
    printf("%s\n", buffer);
    if ((len = getline(&line, &size, stdin)) == -1) continue;
    write(sd, line, len);
  }
  close(sd);
  return 0;
}
