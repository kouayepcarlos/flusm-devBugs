#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <assert.h>

int main(int argc, char ** argv)
{
    printf("Ha!\n");
    assert(argc==2);
    int n = atoi(argv[1]);
    int i=0;
    void *temp = mmap(NULL, 4096, PROT_READ | PROT_WRITE  | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(temp, 'A', 4096);
    while (i<n) {
        i++;
        if (temp) {
            printf("\t%c\n",*(char*)temp);
            munmap(temp,4096);
        }
        else
            printf("NULL given\n");
        sleep(7);
        temp = mmap(NULL, 4096, PROT_READ | PROT_WRITE  | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    return 0;
}
