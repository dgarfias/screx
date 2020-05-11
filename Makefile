all : screx

screx : screx.o 
	mkdir -p ./bin
	$(CC) screx.o /usr/lib/libevdi.so /usr/lib/libvncserver.so -o ./bin/screx

screx.o :
	$(CC) -Wall -g -c screx.c -o screx.o -I./evdi/library

clean :
	rm -f -R bin/ screx.o
