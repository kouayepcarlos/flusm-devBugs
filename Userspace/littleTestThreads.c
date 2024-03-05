#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <pthread.h>

static pthread_mutex_t tMutex;
volatile int counter = 0;

void *test1 (void *arg) {
    int i = 0;
    while (i<10) {
        void *temp = mmap(NULL, 4096, PROT_READ | PROT_WRITE  | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        i++;
        printf("Bef.OneOut!\n");
        if (temp) {
            memset(temp, 'A', 4096);
            munmap(temp,4096);
            //pthread_mutex_lock(&tMutex);
            counter++;
            //pthread_mutex_unlock(&tMutex);
        }
        printf("OneOut!\n");
    }
    printf("Done!\n");
    return NULL;
}

void *test2 (void *arg) {
    int i = 0;
    while (i<10) {
        void *temp = mmap(NULL, 4096, PROT_READ | PROT_WRITE  | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        i++;
        if (temp) {
            memset(temp, 'A', 4096);
            munmap(temp,4096);
            //pthread_mutex_lock(&tMutex);
            counter--;
            //pthread_mutex_unlock(&tMutex);
        }
    }
    return NULL;
}

int main(int argc, char ** argv)
{
    printf("Ha!");
    //pthread_mutex_init(&tMutex, NULL);
    pthread_t tThread1, tThread2;
    pthread_create(&tThread1, NULL, test1, NULL);
    //pthread_create(&tThread2, NULL, test2, NULL);
    //pthread_join(tThread1, NULL);
    sleep(5);
    //pthread_join(tThread2, NULL);
    return 0;
}
