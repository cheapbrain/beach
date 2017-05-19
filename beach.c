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
#include <unistd.h>
#include <stdarg.h>

typedef int16_t i16;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

FILE *logStream;
pthread_mutex_t logMutex;

void mprintf(const char *format, ...) {
  va_list args;
  va_start (args, format);
  pthread_mutex_lock(&logMutex);
  vfprintf(logStream, format, args);
  pthread_mutex_unlock(&logMutex);
  va_end(args);
}

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


#if defined(SERVER)
#define serverMain main
#elif defined(CLIENT)
#define clientMain main
#endif

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

char *configfile = "./config";
char *savefile = "./data";
char *tempfile = "./.temp";

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
  return getYday(-1, tm->tm_year, tm->tm_mon, tm->tm_mday);
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
  char *line;
  size_t bufSize;
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
}


void initBookingList(Season *season) {
  season->bookingList = calloc(season->nUmbrella, sizeof(BookingList));
  for (u32 i = 0; i < season->nUmbrella; i++) {
    pthread_mutex_init(&(season->bookingList[i].mutex), NULL);
  }
}

int odd(i16 n) { return n & 1; }
int even(i16 n) { return !(n & 1); }

void saveBookingList(Season *season) {
  FILE *fp = fopen(tempfile, "w");
  if (fp == NULL) exitError(errno, "failed to open file for writing");

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
    rename(tempfile, savefile);
    fp = fopen(savefile, "r");
  }
  if (fp == NULL) return;

  char *line = NULL;
  size_t bufSize = 0;
  for (u32 i = 0; i < season->nUmbrella; i++) {
    CHECK(getline(&line, &bufSize, fp));

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

void *socketListener(void *commSocket) {
  int csd = *(int *)commSocket;
  int logged = 0;
  struct timeval tv = {0};
  tv.tv_sec = 60;
  setsockopt(csd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(struct timeval));
  char buf[100];
  for (int i = 0; i < 20; i++) {
    int error = read(csd, buf, 100);
    if (error < 1) break;

    char *tokstate;
    char toks[10];
    int nToks = 0;
    toks[0] = strtok_r(buf, " ", &tokstate);
    while(toks[nToks]) {
      nToks++;
      toks[nToks] = strtok_r(NULL, " ", &tokstate);
    }
    if (nToks == 0) continue;
    if (!logged) {
      if (!(strcmp(toks[0], "login"))) {

      } else {
        swrite(csd, "nlogin");
      }
    } else {
      if (!strcmp(toks[0], "book")) {
        if (nToks < 2) {
          swrite(csd, "ok");
          break;
        }
        int nUmbrella = atoi(toks[1]);
        if (nToks < 3) {
          // se disponibile imposta come prenotato
          break;
        }
        int start, end;
        if (nToks < 4) {
          start = getCurrentYday();
          end = parseDate(toks[2]);
        } else {
          start = parseDate(toks[2]);
          end = parseDate(toks[3]);
        }

      } else if (!strcmp(toks[0], "available")) {

      } else if (!strcmp(toks[0], "cancel")) {

      } else {
        swrite(csd, "retry");
      }
    }

    mprintf("%s\n", buf);
  }
  close(csd);
  exitError(0, "disconnesso");
  return NULL;
}

void *socketAccept(void *masterSocket) {
  int msd = *(int *)masterSocket;
  int socket[100];
  int count = 0;
  for (;;) {
    socket[count] = accept(msd, NULL, 0);
    pthread_t child;
    pthread_create(&child, NULL, socketListener, socket + count);
    count++;
  }
  return NULL;
}

void term(int sig) {
  saveBookingList(season);
  mprintf("exit!\n");
  exit(0);
}

int serverMain(int argc, char **argv) {
  signal(SIGINT, term);
  signal(SIGTERM, term);
  signal(SIGQUIT, term);
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(50000);
  sa.sin_addr.s_addr = INADDR_ANY;
  logStream = stdout;
  pthread_mutex_init(&logMutex, NULL);

  season = calloc(1, sizeof(Season));
  loadConfig(season);

  initBookingList(season);
  loadBookingList(season);

  mprintf("%d %d\n", season->start, season->end);

  if (atoi(argv[4]))
    addBooking(season, atoi(argv[5]), atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
  else
    removeBooking(season, atoi(argv[5]), atoi(argv[1]));

  int msd;
  CHECK(msd = socket(AF_INET, SOCK_STREAM, 0));
  CHECK(bind(msd, (struct sockaddr *)&sa, sizeof(sa)));
  listen(msd, 10); //buffer 10 requests
  pthread_t child;
  pthread_create(&child, NULL, socketAccept, &msd);

  pthread_join(child, NULL);
  return 0;
}

void asdf(int sd, char *str) {
  int len = strlen(str) + 1;
  ssize_t tt = write(sd, str, len);
  printf("%zd ", tt);
  usleep(1000);
}

int clientMain(int argc, char **argv) {
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(50000);
  sa.sin_addr.s_addr = inet_addr("127.0.0.1");

  int sd;
  CHECK(sd = socket(AF_INET, SOCK_STREAM, 0));
  CHECK(connect(sd, (struct sockaddr*)&sa, sizeof(sa)));

  while(1) {
    asdf(sd, "book 10\n");
    asdf(sd, "book\nbook 20\nbook 30 12/06/2017\n");
    asdf(sd, "available 3 cancel 2 book book 73 book 74");
    asdf(sd, " book 75 23/06/2017\n");
    printf("\n");
    usleep(1000000);
  }
}
