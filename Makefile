CC = gcc
CFLAGS = -g -std=gnu11 -Werror -Wall -Wextra -Wpedantic -Wmissing-declarations -Wmissing-prototypes -Wold-style-definition -pthread

mdu: mdu.o
	$(CC) $(CFLAGS) -o mdu mdu.o

mdu.o: mdu.c
	$(CC) $(CFLAGS) -c mdu.c

clean:
	rm mdu.o
