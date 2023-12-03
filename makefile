CC	= gcc -g3
CFLAGS  = -g3
TARGET1 = worker
TARGET2 = oss 

OBJS1	= worker.o
OBJS2	= oss.o

all:	$(TARGET1) $(TARGET2)

$(TARGET1):	$(OBJS1)
	$(CC) -o $(TARGET1) $(OBJS1)

$(TARGET2):	$(OBJS2)
	$(CC) -o $(TARGET2) $(OBJS2)

worker.o:	worker.c
	$(CC) $(CFLAGS) -c worker.c 

oss.o:		oss.c
	$(CC) $(CFLAGS) -c oss.c

clean:
	/bin/rm -f *.o $(TARGET1) $(TARGET2)
