OBJS=main.o http_conn.o
CC=g++
CFLAGS+=-c


server:$(OBJS)   
	$(CC) -o server main.o http_conn.o 

main.o:main.cpp http_conn.h locker.h threadpool.h
	$(CC) $(CFLAGS) main.cpp 
http_conn.o:http_conn.cpp http_conn.h
	$(CC) $(CFLAGS) http_conn.cpp 

clean:

	$(RM) *.o server -r
