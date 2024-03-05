#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

void test1 () {
    int i=0;
    while (i<10) {
        void *temp = malloc(4096);
        i++;
        if (temp) {
            memset(temp, 'A', 4096);
            free(temp);
        }
    }
}

int main(int argc, char ** argv)
{
    printf("Ha!");
    //int *x=mmap(NULL, 100, PROT_READ | PROT_WRITE  | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    //test1();
    int n = atoi(argv[1]);
    int i=0;
    while (i<n) {
        void *temp = mmap(NULL, 4096, PROT_READ | PROT_WRITE  | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        i++;
        //char*x=temp;
        //int w=0;
        if (temp) {
            memset(temp, 'A', 4096);
            /*while (w<4096) {
                (*x)++;
                w++;
            }*/
            munmap(temp,4096);
        }
    }
    /*int i=0;
    void*tmp=x;
    while (i<1000) {
    	if (i<100)
    		printf("%c\t",*(char*)x);
    	else
    		printf("%c\t",*(char*)x+61);
    	x++;
    	i++;
    }
    printf("\n");
    printf("Press any key to free\n");
    getchar();
    munmap(tmp, 4*100);
    //printf("%d\n",madvise(tmp, 100, MADV_DONTNEED));
    perror("...");*/
    return 0;
}
