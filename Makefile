

main: main.c net.c log.c utils.c unix.c other.c
	$(CC) $(CFLAGS) -DDEBUG -Os -o geomesh main.c net.c log.c utils.c unix.c other.c

clean:
	rm geomesh