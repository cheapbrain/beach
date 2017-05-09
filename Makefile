FLAGS = -lpthread -g -Wall -Werror -std=c99
SRC = beach.c

all: server client

server: beach.c
	gcc -o server -DSERVER $(SRC) $(FLAGS)
client: beach.c
	gcc -o client -DCLIENT $(SRC) $(FLAGS)

.PHONY: clean

clean:
	rm -f server client
