TARGET = kono
OBJS = main.o
CC = gcc
CFLAGS = -Wall -Wextra -std=c99

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
