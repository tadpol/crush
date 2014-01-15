
#CFLAGS+=-Wall -g -DTEST_IT
CFLAGS+=-Wall -Os -DTEST_IT

all: crush.o
	@ls -lh

clean:
	rm -f *.o crush

