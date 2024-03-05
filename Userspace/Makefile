CC=gcc


all:	usmTagger usmAllocator littleTest littleTestLocks littleTestThreads littleTestMU littleTestEvents

usmTagger: usmTagger.c
	$(CC) usmTagger.c -o usmTagger -lpthread -fPIC -shared -Wall
	
usmAllocator: usmAllocator.c
	$(CC) usmAllocator.c -o usmAllocator -lpthread -Wall -g -O0

littleTest: littleTest.c
	$(CC) littleTest.c -o littleTest -O0 -Wall -pedantic -std=gnu99

littleTestMU: littleTestMU.c
	$(CC) littleTestMU.c -o littleTestMU -O0 -Wall -pedantic -std=gnu99

littleTestLocks: littleTestLocks.c
	$(CC) littleTestLocks.c -o littleTestLocks -O0 -Wall -pedantic -std=gnu99 -lpthread

littleTestThreads: littleTestThreads.c
	$(CC) littleTestThreads.c -o littleTestThreads -O0 -Wall -pedantic -std=gnu99 -lpthread -g -O0

littleTestEvents: littleTestEvents.c
	$(CC) littleTestEvents.c -o littleTestEvents -O0 -Wall -pedantic -std=gnu99

clean:
	$(RM) usmTagger usmAllocator littleTest littleTestLocks littleTestThreads littleTestMU littleTestLocks
