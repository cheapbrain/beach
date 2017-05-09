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
  mprintf("error: %d, %s\nat line: %d", id, msg, lineNumber);
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
  u32 idUmbrella;
  i16 start;
  i16 end;
} Booking;

typedef struct BookingList {
  i16 *startEnd; // even: start, odd: end
  u32 count;
  u32 capacity;
  pthread_mutex_t mutex;
} BookingList;

#define N_ROWS 2
#define N_COLS 5
#define N_UMBRELLA N_ROWS * N_COLS

#define SEASON_START 0
#define SEASON_LENGTH 100

i16 seasonStart;
i16 seasonEnd;

BookingList *bookingList;

char *savefile = "./beach.txt";
char *tempfile = "./.beach.tmp";

void initBookingList() {
  bookingList = calloc(N_UMBRELLA, sizeof(BookingList));
  for (u32 i = 0; i < N_UMBRELLA; i++) {
    pthread_mutex_init(&(bookingList[i].mutex), NULL);
  }
  seasonStart = SEASON_START * 2;
  seasonEnd = seasonStart + SEASON_LENGTH * 2 + 1; 
}

int odd(i16 n) { return n & 1; }
int even(i16 n) { return !(n & 1); }

void saveBookingList() {
  FILE *fp = fopen(tempfile, "w");
  if (fp == NULL) exitError(errno, "failed to open file for writing");

  for (u32 i = 0; i < N_UMBRELLA; i++) {
    BookingList *list = bookingList + i;
    pthread_mutex_lock(&list->mutex);
    i16 *array = list->startEnd;
    u32 count = list->count;
    fprintf(fp, "%d", count);
    for (u32 j = 0; j < count; j++) {
      fprintf(fp, " %d", array[j]);
    }
    fprintf(fp, "\n");
    pthread_mutex_unlock(&list->mutex);
  }
  fclose(fp);

  remove(savefile);
  rename(tempfile, savefile);
}

void loadBookingList() {
  FILE *fp;
  fp = fopen(savefile, "r");
  if (fp == NULL) {
    rename(tempfile, savefile);
    fp = fopen(savefile, "r");
  }
  if (fp == NULL) return;

  char *line = NULL;
  size_t bufSize = 0;
  for (u32 i = 0; i < N_UMBRELLA; i++) {
    CHECK(getline(&line, &bufSize, fp));

    BookingList *list = bookingList + i;
    pthread_mutex_lock(&list->mutex);

    char *tokstate;
    char *token = strtok_r(line, " ", &tokstate);
    if (token == NULL) exitError(i, "error on parsing this line of file.");
    u32 count = atoi(token);
    list->startEnd = realloc(list->startEnd, count * sizeof(i16));
    list->count = count;
    list->capacity = count;
    int expect = 1;
    for (u32 j = 0; j < count; j++) {
      token = strtok_r(NULL, " ", &tokstate);
      if (token == NULL) exitError(i, "too few arguments on this line.");
      i16 time = atoi(token);
      if (even(time) != expect) {
        mprintf("Argument %d of line %d not expected.\n", j, i);
        exit(-1);
      }
      expect = !expect;
      list->startEnd[j] = time;
    }
    if (strtok_r(NULL, " ", &tokstate) != NULL) exitError(i, " too many arguments on this line.");
    pthread_mutex_unlock(&list->mutex);
  }
  fclose(fp);
}

int removeBooking(u32 idUmbrella, i16 start, i16 end) {
  start = start * 2;
  end = end * 2 + 1;
  if (idUmbrella >= N_UMBRELLA) return -1;
  if (start > end)              return -1;
  if (start < seasonStart)      return -1;
  if (end > seasonEnd)          return -1;

  BookingList *list = bookingList + idUmbrella;
  pthread_mutex_lock(&list->mutex);
  i16 *array = list->startEnd;
  u32 count = list->count;
  
  i16 smaller = seasonStart - 1;
  i16 bigger = seasonEnd + 1;
  int i = 0;
  for (; i < count; i++) {
    i16 val = array[i];
    if (val < start) smaller = val;
    else break;
  }
  int j = i;
  for (; j < count; j++) {
    i16 val = array[j];
    if (val > end) { bigger = val; break; }
  }
  
  size_t tsize = (count - j) * sizeof(i16);
  i16 *buffer = NULL;
  if (tsize) buffer = memcpy(malloc(tsize), array + j, tsize);
  count = i + count - j;
  if (even(smaller)) count++;
  if (odd(bigger)) count++;
  list->count = count;

  if (count >= list->capacity) {
    list->capacity *= 2;
    list->startEnd = realloc(list->startEnd, list->capacity * sizeof(i16));
    array = list->startEnd;
  }

  if (even(smaller)) array[i++] = start - 1;
  if (odd(bigger)) array[i++] = end + 1;
  if (tsize) memcpy(array + i, buffer, tsize);
  free(buffer);
  pthread_mutex_unlock(&list->mutex);
  return 0;
}

int _testSetBooking(u32 idUmbrella, i16 start, i16 end, int testOnly) {
  start = start * 2;
  end = end * 2 + 1;
  if (idUmbrella >= N_UMBRELLA) return -1;
  if (start > end)              return -1;
  if (start < seasonStart)      return -1;
  if (end > seasonEnd)          return -1;

  BookingList *list = bookingList + idUmbrella;
  pthread_mutex_lock(&list->mutex);
  i16 *array = list->startEnd;
  u32 count = list->count;
  int i = 0;
  for (; i < count; i++) {
    i16 val = array[i];
    if (val < start) continue;
    else if (val == start) goto fail;
    else if (val < end || odd(val)) goto fail;
    else if (testOnly) goto success;
    else break;
  }

  list->count += 2;
  if (list->count > list->capacity) {
    list->capacity *= 2;
    if (list->capacity == 0) list->capacity = 4;
    list->startEnd = realloc(list->startEnd, list->capacity * sizeof(i16));
    array = list->startEnd;
  }
  if (i < count)
    memmove(array + i + 2, array + i, sizeof(i16) * count);
  array[i] = start;
  array[i+1] = end;
success:
  pthread_mutex_unlock(&list->mutex);
  return 0;
fail:
  pthread_mutex_unlock(&list->mutex);
  return -1;
}

//returns 0 if the booking is available
int testBooking(u32 idUmbrella, i16 start, i16 end) {
  return _testSetBooking(idUmbrella, start, end, 1);
}
//returns 0 if the booking is successful
int addBooking(u32 idUmbrella, i16 start, i16 end) {
  return _testSetBooking(idUmbrella, start, end, 0);
}

void *socketListener(void *commSocket) {
  int csd = *(int *)commSocket;
  char buf[100];
  for (;;) {
    read(csd, buf, 100);
    mprintf("%s\n", buf);
  }
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
  saveBookingList();
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

  initBookingList();
  loadBookingList();

  if (atoi(argv[4]))
    addBooking(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
  else
    removeBooking(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
  term(0);

  int msd;
  CHECK(msd = socket(AF_INET, SOCK_STREAM, 0));
  CHECK(bind(msd, (struct sockaddr *)&sa, sizeof(sa)));
  listen(msd, 10); //buffer 10 requests
  pthread_t child;
  pthread_create(&child, NULL, socketAccept, &msd);

  pthread_join(child, NULL);
  return 0;
}

int clientMain(int argc, char **argv) {
  char *msg = argv[1];
  int msgLen = strlen(msg) + 1;
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(50000);
  sa.sin_addr.s_addr = inet_addr("127.0.0.1");

  int sd;
  CHECK(sd = socket(AF_INET, SOCK_STREAM, 0));
  CHECK(connect(sd, (struct sockaddr*)&sa, sizeof(sa)));

  while(1) {
    write(sd, msg, msgLen);
    usleep(10000000);
  }
}
