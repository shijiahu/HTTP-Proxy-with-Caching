all:
	gcc -o proxy proxy.c -lpthread
clean:
	rm proxy