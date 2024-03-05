#include <stdio.h>
#include <stdlib.h>
#include <linux/kernel.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "list.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define PAGE_SIZE sysconf(_SC_PAGESIZE)

static const unsigned long usmMemSize = (unsigned long) 9*1024*1024*1024;

static pthread_mutex_t fdMutex;
static pthread_mutex_t pagesRelatedMutex;
static struct process {
    int fd;
    struct process * next;
} * processList = NULL;
static struct process * lastProcessEntry = NULL;

static struct processDesc {
    int fd;
    //int initPages;
} *processes;

/*static struct eludeList {                               // LRU'd be at the start.           | For some fine-grained affinity search, duplicate for example this, with some timestamp, per descriptor
    struct page * address;
    struct eludeList * next;
    struct eludeList * prev;
} * freeList;
static volatile struct eludeList * lastFreeListEntry;*/

//static struct eludeList * usedList = NULL;
struct optEludeList {
    struct page * address;
    struct list_head iulist;
};
struct list_head usedList;
LIST_HEAD(usedList);
struct list_head freeList;
LIST_HEAD(freeList);
//static int usedListCount = 0;                     interesting, no need to +- anymore... supposing there's no HP

//static volatile struct eludeList * lastUsedListEntry = NULL;

static struct page {
    intptr_t data;
    unsigned long virtualAddress;
    unsigned long physicalAddress;
    int descriptor;
    struct list_head * usedListPrev;
    struct optEludeList * usedListCt;

} *pagesList;

static void * usmZone = NULL;
static unsigned int globalPagesNumber;
static unsigned int basePFN;
static volatile unsigned long usedMemory = 0;


static inline void insertPage (struct page ** pagesList, unsigned long physicalAddress, intptr_t data, int pos) {
    // Some globalPagesNumber ver. and new malloc...
    ((*pagesList)+pos)->data = data;
    ((*pagesList)+pos)->physicalAddress = physicalAddress;
    ((*pagesList)+pos)->virtualAddress = 0;
    ((*pagesList)+pos)->descriptor = 0;
    //((*pagesList)+pos)->usedListPosition = NULL;
}

static inline void listPages (struct page * pagesList, int number) {
    printf("List : \n");
    while (number>0) {
        printf("\t%lu, %lu, %lu, %d\n", pagesList->data, pagesList->physicalAddress, pagesList->virtualAddress, pagesList->descriptor);
        pagesList++;
        number--;
    }
    printf("End.\n");
}

static inline void printPages (struct page * pagesList, int number) {
    FILE * f = fopen("Hmm", "w");
    while (number>0) {
        fprintf(f, "%p\n", pagesList);
        pagesList++;
        number--;
    }
    fclose(f);
}

/*static inline struct page * findFreePage (struct eludeList ** freeList) {    // popFreePage
    printf("FindFP %p\n", (*freeList)->address);
    struct page * ret = (struct page *) (*freeList)->address;
    struct eludeList *temp = *freeList;
    *freeList=(*freeList)->next;
    free(temp);
    printf("\tret: %p\n", ret);
    if((*freeList) == NULL)
        printf("FindFPNNow NULL !\n");
    else
        printf("FindFPN %p\n", (*freeList)->address);
    return ret;
}*/
static inline struct page * findFreePage() {    // popFreePage
    struct optEludeList * temp = list_first_entry(&freeList, struct optEludeList, iulist); //(struct page *) (*freeList)->address;
    struct page *ret = temp->address;
    list_del(&(temp->iulist));
    free(temp);
    return ret;
}

/*
 *** This was to be combined with descriptor, optimized and so on and used on an unmap&co event to find which page to mark free (essentially putting relevant fields back to 0)...
    inline struct page * findCorrespondingPage (struct page ** pagesList, unsigned long virtualAddress) {
    struct page * current = *pagesList;
    while (current) {
        if (current->virtualAddress == virtualAddress)
            return current;
        current=current->next;
    }
    return NULL;
}*/

// Takes a random page to swap out : should be part of Caroline's core interest     #LegacyLinkedListStructPage
/*static inline struct page * findUsedPage (struct page ** pagesList) {
    struct page * current = *pagesList;
    struct page * prev = NULL;
    while (current) {
        if (current->virtualAddress) {
            if (prev != NULL)
                prev->next=current->next;
            else
                *pagesList = current->next;
            return current;
        }
        prev = current;
        current=current->next;
    }
    return NULL;
}*/

// Takes care of waking processes and adding their descriptors to a list consumed by main (Allocator)
static void *processesHandler(void *arg) {
    int res = 0;
usminit:
    int usmfd = open("/proc/usm", O_RDWR);
    printf("usmfd : %d\n", usmfd);
    if (usmfd < 0) {
        perror("\t[mayday/processesHandler] usmfd not available! Try again?\n");
        getchar();
        goto usminit;
    }
    int processfd = 0;
    char procName [16];

    struct pollfd pollfd[1];
    pollfd[0].fd = usmfd;
    pollfd[0].events = POLLIN;
    while (1) {
        res=poll(pollfd, 1, 0);
        if (res==0)
            continue;
        // Retrieval of the descriptor
        processfd = read(usmfd, &procName, 16);
        if (processfd <= 0) {
            if (errno == EAGAIN)
                continue;
            perror("\t[mayday/processesHandler] Reading error! Continue?\n");
            getchar();
            continue;
        }
        // Waking of the relevant process
        printf("Waking %s's process\n", procName);
        write(usmfd, NULL, 0);
        pthread_mutex_lock (&fdMutex);
        if (lastProcessEntry!=NULL) {
            lastProcessEntry->next=malloc(sizeof(struct process));
            lastProcessEntry=lastProcessEntry->next;
            lastProcessEntry->fd=processfd;
            lastProcessEntry->next=NULL;
        } else {
            if (processList!=NULL) {
                lastProcessEntry->next=malloc(sizeof(struct process));
                lastProcessEntry=lastProcessEntry->next;
                lastProcessEntry->fd=processfd;
                lastProcessEntry->next=NULL;
            } else {
                processList=malloc(sizeof(struct process));
                processList->fd=processfd;
                processList->next=NULL;
                lastProcessEntry=processList;
            }
        }
        pthread_mutex_unlock (&fdMutex);
    }
    return NULL;
}

// Takes care of marking pages free
static void *pagesHandler(void *arg) {
    int res = 0;
usminit:
    int usmfd = open("/proc/usmPgs", O_RDWR);
    printf("usmPgsfd : %d\n", usmfd);
    if (usmfd < 0) {
        perror("\t[mayday/pagesHandler] usmPgsfd not available! Try again?\n");
        getchar();
        goto usminit;
    }
    unsigned long pfn = 0;
    unsigned long resultingPN = 0;                         // -1 then further ver.s
    struct optEludeList * tempEntry = (struct optEludeList *)malloc(sizeof(struct optEludeList));

    struct pollfd pollfd[1];
    pollfd[0].fd = usmfd;
    pollfd[0].events = POLLIN;
    while (1) {
        res=poll(pollfd, 1, 5);
        if (res==0)
            continue;
        // Retrieval of the PFN
        res = read(usmfd, &pfn, sizeof(pfn));
        if (res <= 0) {
            if (errno == EAGAIN)
                continue;
            perror("\t[mayday/pagesHandler] Reading error! Continue?\n");
            getchar();
            continue;
        }
        printf("Received %lu ! #freePage\n", pfn);
        resultingPN = pfn-basePFN;
        if (unlikely(resultingPN >= globalPagesNumber || pfn < basePFN)) {
            printf("Bug %lu ! #freePage\n", resultingPN);
            continue;
        }

        pthread_mutex_lock(&pagesRelatedMutex);
                tempEntry->address=pagesList+resultingPN;
        memset((void *)(pagesList+resultingPN)->data, 0, 4096);
        if(tempEntry->address == NULL) {
            printf("\t[mayday/pagesHandler] NULL address..\n\n\n\n");
            getchar();
        }
        printf("\t%lu\n", ((struct page *) (tempEntry->address))->physicalAddress);
        list_add(&(tempEntry->iulist),&freeList);
        pagesList[resultingPN].virtualAddress=0;
        pagesList[resultingPN].descriptor=0;
        if ((pagesList[resultingPN].usedListPrev)==NULL || ((pagesList[resultingPN].usedListCt))==NULL){
            pthread_mutex_unlock(&pagesRelatedMutex);
            printf("[mayday/pagesHandler] Received one unlogical : %lu #freePage\n", resultingPN);
            continue;
        }
        __list_del(pagesList[resultingPN].usedListPrev, (pagesList[resultingPN].usedListCt)->iulist.next);
        free(pagesList[resultingPN].usedListCt);
        //listPages(pagesList, globalPagesNumber);
        usedMemory-=PAGE_SIZE;
        printf("Memory consumed : %lu KB #freePage\n", usedMemory/1024);
        pthread_mutex_unlock(&pagesRelatedMutex);
        tempEntry=(struct optEludeList *)malloc(sizeof(struct optEludeList));
        INIT_LIST_HEAD(&(tempEntry)->iulist);
        if (tempEntry == NULL) {
            printf("Wait wut !?\n");
            getchar();
        }
    }
    return NULL;
}

int main() {
    int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    int res = 0;
    if (uffd == -1) {
        perror("syscall/userfaultfd");
        exit(1);
    }
    struct uffdio_api uffdio_api;
    uffdio_api.api = UFFD_API;
    uffdio_api.features = 0;
    if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
        perror("\t\t[Mayday] UFFD_API ioctl failed! Exiting.");
        exit(1);
    }

    struct uffdio_palloc uffdio_palloc;
    uffdio_palloc.opt = 3;

    pthread_t processesHandlerThread;
    pthread_t pagesHandlerThread;
    pthread_mutex_init(&fdMutex, NULL);
    pthread_mutex_init(&pagesRelatedMutex, NULL);

usminit:
    int usmCMA = open("/dev/USMMcd", O_RDWR);
    printf("usmCMAfd : %d\n", usmCMA);
    if (usmCMA < 0) {
        perror("\t[mayday/pagesHandler] usmCMAfd not available! Try again?\n");
        getchar();
        goto usminit;
    }

    // Allocating our zone from our module's CMA memory ; known bug with PROT_EXEC
    usmZone = mmap(NULL, usmMemSize, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_SHARED, usmCMA, 0);
    if(usmZone==MAP_FAILED) {
        perror("\t\t[Mayday] mmap failed! Exiting.");
        exit(1);
    }
    printf("Was .. %d %d %d ; sanitizing\n", *(char*)usmZone, *(char*)(usmZone+1), *(char*)(usmZone+2));
    //bzero(usmZone, usmMemSize);
    memset(usmZone, 0, usmMemSize);


    uffdio_palloc.addr = (unsigned long) usmZone;
    globalPagesNumber=usmMemSize/PAGE_SIZE;
    uffdio_palloc.uaddr = (unsigned long) globalPagesNumber;
    // Getting our first PFN ; done with UFFDIO_PALLOC option 3 (other ones : 1 to merely check on presence, 4 to modify/alter/install a PTE, notably on pagefaults, ...)
    res = ioctl(uffd, UFFDIO_PALLOC, &uffdio_palloc);
    if (res < 0) {
        perror("\t\t[Mayday] First PFN query failed! Exiting.");
        exit(1);
    }
    printf("Range : %lld | %lld\n", uffdio_palloc.uaddr,uffdio_palloc.uaddr+globalPagesNumber);
    basePFN=uffdio_palloc.uaddr;
    pagesList=(struct page *) malloc (sizeof(struct page)*globalPagesNumber);

    // Inserting the pages in our structs ; no more ioctl needed, as pages known to be ordered (CMA)
    for (int i = 0; i<globalPagesNumber; i++) {
        /*char * usmPage = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_SHARED, usmCMA, 0);
        if(usmPage==MAP_FAILED) {
            perror("\t\t[Mayday] mmap failed! Exiting.");
            exit(1);
        }
        memset(usmPage, 0, PAGE_SIZE);*/
        // insertPage(&pagesList, uffdio_palloc.uaddr+i, (intptr_t)(usmPage), i);
        insertPage(&pagesList, uffdio_palloc.uaddr+i, (intptr_t)(usmZone+i*PAGE_SIZE), i);
        struct optEludeList *freeListNode=(struct optEludeList *)malloc(sizeof(struct optEludeList));
        freeListNode->address=pagesList+i;
        INIT_LIST_HEAD(&(freeListNode->iulist));
        list_add_tail(&(freeListNode->iulist),&freeList);
    }
    //freeList->prev=lastFreeListEntry;
    printPages(pagesList, globalPagesNumber);
    //usedList=(struct eludeList *)malloc(sizeof(struct eludeList));
    //usedList->prev=NULL;
    //lastUsedListEntry=usedList;

    pthread_create(&processesHandlerThread, NULL, processesHandler, NULL);
    pthread_create(&pagesHandlerThread, NULL, pagesHandler, NULL);

    struct pollfd fds[1];
    int nfds = 0;
    memset(fds, 0, sizeof(fds));

    processes=(struct processDesc *) malloc (sizeof(struct processDesc)*1000);

    // First loop simply not to start any polling until a relevant situation calls for it : a managed process knocks at the door
    while (1) {
        pthread_mutex_lock (&fdMutex);
        if(processList!=NULL) {
            printf("Treating one friend!\n");
            while(processList!=NULL) {
                fds[nfds].fd = processList->fd;
                fds[nfds].events = POLLIN;
                //processes[fds[nfds].fd].initPages=0;
                /*if (ioctl(fds[nfds].fd, UFFDIO_TAG, NULL)!=0) {
                	perror("[mayday/mayday] TAG nope");
                	getchar();
                }*/
                nfds++;
                struct process *processEntry = processList->next;
                free(processList);
                processList=processEntry;
            }
            printf("\tDone!\n\n");
            lastProcessEntry=processList;
            break;
        }
        pthread_mutex_unlock (&fdMutex);
    }
    pthread_mutex_unlock (&fdMutex);

    fds[0].events = POLLIN;
    int current_size;

    struct uffd_msg msg;
    struct page * chosenPage = NULL;
    uffdio_palloc.opt = 4;

    // Main loop, polling on userfaultfd events per process
    while(1) {
        // Always checking if any new friend knocked (through the corresponding thread), and adding it/them to the polled descriptors
        pthread_mutex_lock (&fdMutex);
        if (processList!=NULL) {
            printf("Treating one friend!\n");
            while(processList!=NULL) {
                //if (nfds==363)
                fds[nfds].fd = processList->fd;
                //processes[fds[nfds].fd].initPages=0;
                fds[nfds].events = POLLIN;
                nfds++;
                struct process *processEntry = processList->next;
                free(processList);
                processList=processEntry;
            }
            lastProcessEntry=processList;
            printf("\tDone!\n\n");
        }
        pthread_mutex_unlock (&fdMutex);
        res = poll(fds, nfds, 0);
        if (res < 0) {
            perror("\t\t[Mayday] Faults polling failed! Exiting.\n");
            exit(1);
        }
        if (res == 0)
            continue;
        current_size = nfds;
        for (int i = 0; i < current_size; i++) {
            if(fds[i].events == 0 && fds[i].revents == 0)
                continue;
            // Events different from POLLIN ones're assumed to be process' death ; we hence simply remove it from our poll list
            if(fds[i].events != POLLIN || fds[i].revents != POLLIN) {
                printf("[Goodbye] Process %d just reached the afterworld.\n", fds[i].fd);
                //close(fds[i].fd);
                //processes[fds[i].fd].initPages=0; Nah'Need.
                fds[i].fd = -1;
                for (int i = 0; i < nfds; i++) {
                    if (fds[i].fd == -1) {
                        for(int j = i; j < nfds; j++) {
                            fds[j].fd = fds[j+1].fd;
                        }
                        i--;
                        nfds--;
                    }
                }
                continue;
            }

            int readres = read(fds[i].fd, &msg, sizeof(msg));
            if (unlikely (readres == -1)) {
                if (errno == EAGAIN)
                    continue;
                perror("\t\t[Mayday] Failed msg read! Exiting.\n");
                exit(1);
            }
            // Handle the page fault by appointing one of our pages to the corresponding address
            if (likely(msg.event == UFFD_EVENT_PAGEFAULT)) {
                printf("\tHandling fault with flag : %llx | %d\n", msg.arg.pagefault.flags, fds[i].fd);
                uffdio_palloc.addr = msg.arg.pagefault.address;
                pthread_mutex_lock(&pagesRelatedMutex);
                if(list_empty(&freeList)) {
                    printf("[Mayday] Memory full (fl)!\n");
                    exit(1);
                }
                chosenPage=findFreePage(&freeList);
                pthread_mutex_unlock(&pagesRelatedMutex);
                if (chosenPage==NULL) {
                    // Checking once again.. after a millisecond... and if not : (!)
                    printf("[Mayday] Memory full (cp)!\n");
                    exit(1);
                }
                pthread_mutex_lock(&pagesRelatedMutex);
                uffdio_palloc.uaddr = chosenPage->physicalAddress;
                pthread_mutex_unlock(&pagesRelatedMutex);
                if (ioctl(fds[i].fd, UFFDIO_PALLOC, &uffdio_palloc) == -1) {
                    perror("\t\t[Mayday] Allocation failed!");
                    //exit(1);
                    continue;
                }
                struct optEludeList *usedListNode=(struct optEludeList *)malloc(sizeof(struct optEludeList));
                usedListNode->address=chosenPage;
                INIT_LIST_HEAD(&(usedListNode->iulist));
                list_add(&(usedListNode->iulist),&usedList);

                pthread_mutex_lock(&pagesRelatedMutex);
                //chosenPage->usedListPosition=(struct eludeList *)lastUsedListEntry;
                chosenPage->usedListPrev=usedListNode->iulist.prev;
                chosenPage->usedListCt=usedListNode;
                chosenPage->virtualAddress=msg.arg.pagefault.address;
                //printf("Given : %lu!\n", chosenPage->physicalAddress);
                /*if(chosenPage->descriptor!=0)
                    printf("Holy mayday!\n");*/
                chosenPage->descriptor=fds[i].fd;
                if(unlikely(fds[i].fd>1000)) {
                    printf("Unsupported number of processes\n");
                    exit(1);
                }
                /*if(unlikely(processes[fds[i].fd].initPages<2)) {
                    memset((void*)chosenPage->data, 0, PAGE_SIZE);                                             // Hyper not the place... | at least pool to be prepared (!)
                    processes[fds[i].fd].initPages++;
                }*/
                usedMemory+=PAGE_SIZE;
                chosenPage->virtualAddress=uffdio_palloc.addr;
                //listPages(pagesList, globalPagesNumber);
                printf("Memory consumed : %lu KB\n", usedMemory/1024);
                pthread_mutex_unlock(&pagesRelatedMutex);
                continue;
            } else {
                if (msg.event == UFFD_EVENT_UNMAP) {
                    printf("\tUnmap event : %llu! | %d\n", msg.arg.remove.start, fds[i].fd);
                    continue;
                }
                if (msg.event == UFFD_EVENT_FORK) {
                    printf("\tFork event : %u! | %d\n", msg.arg.fork.ufd, fds[i].fd);
                    // The fork event gives the newly created userfault file descriptor ; we'd add it to our polled ones, and tag it (i.e. put VM_UFFD_MISSING into its mm's def_flags)
                    pthread_mutex_lock (&fdMutex);
                    if (processList==NULL) {
                        processList=malloc(sizeof(struct process));
                        processList->next=NULL;
                        processList->fd=msg.arg.fork.ufd;
                        lastProcessEntry=processList;
                    }
                    else {
                        lastProcessEntry->next=malloc(sizeof(struct process));   //t->next=processList;
                        lastProcessEntry=lastProcessEntry->next;//lastProcessEntry=t;
                        lastProcessEntry->fd=msg.arg.fork.ufd;
                        lastProcessEntry->next=NULL;
                    }
                    pthread_mutex_unlock (&fdMutex);
                    /*if (ioctl(fds[nfds].fd, UFFDIO_TAG, &uffdio_palloc)!=0) {
                        perror("[mayday/mayday] TAG nope");
                        getchar();
                    }*/
                    printf("D'Done!\n");
                    //getchar();
                    continue;
                }
                if (msg.event == UFFD_EVENT_REMOVE) {
                    printf("\tRemove event : %llu! | %d\n", msg.arg.remove.start, fds[i].fd);
                    continue;
                }
                if (msg.event == UFFD_EVENT_REMAP) {
                    printf("\tRemap event %llu! | %d\n", msg.arg.remap.len, fds[i].fd);
                    continue;
                }
                printf("\t\tUnassumed event! | %d\n", fds[i].fd);
                continue;
            }
        }
    }

    return 0;
}
