#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <semaphore.h>
#include <pthread.h>
#include <inttypes.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

void exitError(int id, char *msg) {
  printf("%s\n", msg);
  exit(id);
}

typedef struct JobDesc {
  int jobId;
  int arrivalTime;
  int burstLenght;
  int priority;
  int startTime;
  int endTime;
  int remainingTime;
  int turnaroundTime;
} JobDesc;

typedef struct Event {
  int
typedef struct SafeQueue {
  void *buffer;
  size_t elementSize;
  u32 capacity;
  u32 count;
  u32 head;
} SafeQueue;

enum Params {
  P_MEMORY = 1,
  P_QUANTO,
  P_FILE,
  _N_PARAMS
};

{{
void parsingThread(char *filename) {
  FILE *fp = NULL;
  fp = fopen(filename, "r");
  if (fp == NULL) exitError(1, "file not found.\n");

  char *line;
  size_t lineLen;
  int lineCount = 0;  
  while(getline(&line, &lineSize, fp) != -1) {
    lineCount++;
    char *token = strtok(line, " =\t");
    int expectedTokens = 0;
    
    if (
  }

  free(line);
}

int main(int argc, char **argv) {
  if (argc != _N_PARAMS) exitError(1, "usage: ./scheduler M Q FILE\n");

  char str[256] = "one two,three";

  char *one = strtok(str, " ,");
  char *two = strtok(NULL, " ,");
  char *three = strtok(NULL, " ,");
  printf("%s.%s.%s\n", one, two, three);

  printf("%dasjdfsadjlf\n", FOPEN_MAX);
  usleep(10000000);
  printf("asdflafjsjlfd\n");

  return 0;
}
