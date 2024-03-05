#include "../../../include/usm/usm.h"

/*
    Basic allocation functions serving as examples. One is required to take a lock whe(r|n)ever touching structures, as those are shared between threads.
        Per page locking proposed through -DPLock compilation option, but really not recommended.
    optEludeList can be changed to any type one'd like, but the related placeholder's quite needed (or any other method one deems wanted) to efficiently mark freed pages as such.
    The same goes for any related point. One has unlimited freedom, just with some logic in linked matters in the API we recommend to read.
*/

/*
    pthread_t internalLocks; -> to initialize in defined usm_setup....
*/

/*
    If you really don't want to use the position pointer, or add more in the case of multiple levels implied, feel free to modify usm's proposed page structure in usm.h.
    Just try not to heavify it too much.
*/




/*
    One dumb idea : reserving pools through defined functions appointed to different policies mapped to processes through the config. file (x set of functions treats of one portion of USM's memory and the other ones the rest of the latter)...

*/

struct optEludeList {
    struct page * usmPage;      // no pointer needed..
    struct list_head iulist;
};

struct list_head freeList;
struct list_head usedList;

pthread_mutex_t policiesSet1lock;

LIST_HEAD(usedList);
LIST_HEAD(freeList);

struct usm_ops dev_usm_ops;         // just one for now but can be greater, i.e. multiple USM instances per arbitrary policies....



static inline int basic_alloc(struct usm_event *event) {
    pthread_mutex_lock(&policiesSet1lock);
    if (list_empty(&freeList)) {
        pthread_mutex_unlock(&policiesSet1lock);       // func...
        return 1;
    }
    struct optEludeList * freeListNode = list_first_entry(&freeList, struct optEludeList, iulist);
    usedMemory+=globalPageSize;         // though.. if we just simply let the leveraging of differently defined page sizes to developers... it shouldn't be gud.. so we'll just call their functions a number globalPageSize/SYS_PAGE_SIZE of times from USM.. TODO
    list_move(&(freeListNode->iulist),&usedList);
    pthread_mutex_unlock(&policiesSet1lock);
    if(freeListNode->usmPage == NULL) {
#ifdef DEBUG
        printf("[devPolicies/Mayday] NULL page..! Aborting.\n");
        getchar();
        exit(1);
#endif
        return 1;
    }
    usmSetUsedListNode(freeListNode->usmPage, freeListNode); // freeListNode->usmPage->usedListPositionPointer=freeListNode;
    event->paddr=freeListNode->usmPage->physicalAddress;            // we'd need the last one... buffer adding?
#ifdef DEBUG
    printf("[devPolicies] Chosen addr.:%lu\n",event->paddr);
    if (event->paddr==0)
        getchar();
#endif
    //retry:
    if(usmSubmitAllocEvent(event)) {                // will be multiple, i.e. multiplicity proposed by event, and directly applied by usmSubmitAllocEvent
#ifdef DEBUG
        printf("[devPolicies/Mayday] Unapplied allocation\n");
        getchar();
#endif
        pthread_mutex_lock(&policiesSet1lock);
        list_move(&(freeListNode->iulist),&freeList);
        usedMemory-=globalPageSize;         // though.. if we just simply let the leveraging of differently defined page sizes to developers... it shouldn't be gud.. so we'll just call their functions a number globalPageSize/SYS_PAGE_SIZE of times from USM.. TODO
        pthread_mutex_unlock(&policiesSet1lock);
        return 1;
        //goto retry; // some number of times...
    }
    usmLinkPage(freeListNode->usmPage,event);
    /*freeListNode->usmPage->virtualAddress=event->vaddr;
    freeListNode->usmPage->process=event->origin;*/
    increaseProcessAllocatedMemory(event->origin, globalPageSize);
    // ... if recognized pattern.. multiple submits.. and so on....             + VMAs list and size on the way! -> no needless long checks if applicable!
    return 0;
}

static inline int double_alloc(struct usm_event *event) {
    if (basic_alloc(event))
        return 1;
    event->vaddr+=SYS_PAGE_SIZE;        // spatial distrib. file could tell the next vaddr to allocate!
    if (basic_alloc(event)) {         // second one's not obliged to work... i.e. VMA's full..&Co.
        pthread_mutex_lock(&policiesSet1lock);
        list_move(&((struct optEludeList *)usmPfnToUsedListNode(usmPfnToPageIndex(event->paddr)))->iulist,&freeList); //list_move(&((struct optEludeList *)usmPageToUsedListNode(usmEventToPage(event)))->iulist,&freeList);
        pthread_mutex_unlock(&policiesSet1lock);
        usmResetUsedListNode(usmEventToPage(event)); //pagesList[usmPfnToPageIndex(event->paddr)].usedListPositionPointer=NULL;
#ifdef DEBUG
        printf("[devPolicies/Sys] Unapplied allocation.. though not mandatory.\n");
        getchar();
#endif
    }
    return 0;
}

/*static inline struct page * pair_pages_alloc() {
    pthread_mutex_lock(&policiesSet1lock);
    if (list_empty(&freeList)){
        pthread_mutex_unlock(&policiesSet1lock);
        return NULL;
    }
    struct optEludeList * freeListNode = list_first_entry(&freeList, struct optEludeList, iulist);
    while ((freeListNode->usmPage)->physicalAddress%2!=0) {
        if (list_empty(&(freeListNode)->iulist)){
            pthread_mutex_unlock(&policiesSet1lock);
            return NULL;
        }
        freeListNode=list_first_entry(&(freeListNode)->iulist, struct optEludeList, iulist);
    }
    usedMemory+=SYS_PAGE_SIZE;
    freeListNode->usmPage->usedListPositionPointer=freeListNode;    // toMove....
    list_move(&(freeListNode->iulist),&usedList);
    pthread_mutex_unlock(&policiesSet1lock);

    //ret->virtualAddress=va;

    return freeListNode->usmPage;
}*/

int reverse_alloc(struct usm_event *event) {
#ifdef DEBUG
    printf("[devPolicies]Reversing!\n");
    //getchar();
#endif
    pthread_mutex_lock(&policiesSet1lock);
    if (list_empty(&freeList)) {
        pthread_mutex_unlock(&policiesSet1lock);
        return 1;
    }
    struct optEludeList * freeListNode = list_entry(freeList.prev, struct optEludeList, iulist);
    usedMemory+=globalPageSize;
    freeListNode->usmPage->usedListPositionPointer=freeListNode;
    list_move(&(freeListNode->iulist),&usedList);
    pthread_mutex_unlock(&policiesSet1lock);
    event->paddr=freeListNode->usmPage->physicalAddress;
#ifdef DEBUG
    printf("[devPolicies]Chosen addr.:%lu\n",event->paddr);
    if(event->paddr==0)
        getchar();
#endif
    if(usmSubmitAllocEvent(event)) {
#ifdef DEBUG
        printf("[devPolicies/Mayday] Unapplied allocation\n");
        getchar();
#endif
        pthread_mutex_lock(&policiesSet1lock);
        list_move(&(freeListNode->iulist),&freeList);
        usedMemory-=globalPageSize;
        pthread_mutex_unlock(&policiesSet1lock);
        return 1;
    }
    increaseProcessAllocatedMemory(event->origin, globalPageSize);
    freeListNode->usmPage->virtualAddress=event->vaddr;
    freeListNode->usmPage->process=event->origin;
    return 0;
}

static inline int basic_free(struct usm_event * event) {
    int ret = 0;
    struct optEludeList * tempEntry;
    list_for_each_entry(tempEntry,&usedList,iulist) {                       // per process.... DI.
        if (tempEntry->usmPage->virtualAddress==event->vaddr)
            break;
    }
    if(&tempEntry->iulist==&usedList) {
        printf("[devPolicies/Mayday] Corresponding page not found!\n");
        ret=1;
        goto out;
    }
    pthread_mutex_lock(&policiesSet1lock);
    list_move(&tempEntry->iulist,&freeList);
    usedMemory-=SYS_PAGE_SIZE;
    pthread_mutex_unlock(&policiesSet1lock);
    memset((void*)((tempEntry->usmPage)->data), 0, 4096);
#ifdef DEBUG
    printf("\t%lu | %lu\n", event->vaddr, ((struct page *) (tempEntry->usmPage))->physicalAddress);
#endif
    tempEntry->usmPage->virtualAddress=0;
    decreaseProcessAllocatedMemory(tempEntry->usmPage->process, SYS_PAGE_SIZE);
    tempEntry->usmPage->process=0;
    tempEntry->usmPage->usedListPositionPointer=NULL;
out:
    return ret;
}

static inline int pindex_free(struct usm_event * event) {
    struct page * usmPage = usmEventToPage(event);
    if(unlikely(!usmPage)) {
#ifdef DEBUG
        printf("[devPolicies/Mayday] Event corresponding to no page\n");
#endif
        return 1;
    }
    if (unlikely(!usmPageToUsedListNode(usmPage))) {
        printf("[devPolicies/Sys] Calling basic free!\n");
        getchar();
        event->vaddr=usmPage->virtualAddress;
        return basic_free(event);
    }
    memset((void*)(usmPage->data), 0, 4096);
#ifdef DEBUG
    printf("\t[devPolicies]Collecting freed %lu\n", usmPage->physicalAddress);
#endif
    pthread_mutex_lock(&policiesSet1lock);
    list_move_tail(&((struct optEludeList *)usmPageToUsedListNode(usmPage))->iulist,&freeList);
    usmPage->virtualAddress=0;
    decreaseProcessAllocatedMemory(usmPage->process, SYS_PAGE_SIZE);     // can and will always only be of SYS_PAGE_SIZE...
    usmPage->process=0;
    usedMemory-=SYS_PAGE_SIZE;
    pthread_mutex_unlock(&policiesSet1lock);
    return 0;
}






struct usm_alloc_policy_ops usm_basic_alloc_policy= {.usm_alloc=basic_alloc,.usm_pindex_free=pindex_free,.usm_free=basic_free};
struct usm_alloc_policy_ops alloc_policy_one= {.usm_alloc=reverse_alloc, .usm_pindex_free=pindex_free,.usm_free=basic_free};
struct usm_alloc_policy_ops alloc_policy_double= {.usm_alloc=double_alloc, .usm_pindex_free=pindex_free,.usm_free=basic_free};

int policiesSet1_setup(unsigned int pagesNumber) {         // alloc.* setup..
    for (int i = 0; i<pagesNumber; i++) {
        struct optEludeList *freeListNode=(struct optEludeList *)malloc(sizeof(struct optEludeList));
        freeListNode->usmPage=pagesList+i;  // param.
        INIT_LIST_HEAD(&(freeListNode->iulist));
        list_add(&(freeListNode->iulist),&freeList);
    }
    if(usm_register_alloc_policy(&alloc_policy_one,"policyOne",false))
        return 1;
    if(usm_register_alloc_policy(&usm_basic_alloc_policy,"basicPolicy",true))
        return 1;
    if(usm_register_alloc_policy(&alloc_policy_double,"doublePolicy",false))
        return 1;
    pthread_mutex_init(&policiesSet1lock,NULL);
    // pthread_create... of any policy/locally defined thread doing anything upon live stat.s proposed by USM
    return 0;
}

void pol_structs_free() {
    struct list_head *listIterator, *tempLstIterator;
    list_for_each_safe(listIterator,tempLstIterator,&(freeList)) {
        struct optEludeList * listNode=list_entry(listIterator, struct optEludeList, iulist);
        list_del(listIterator);
        free(listNode->usmPage);
        free(listNode);
    }
    list_for_each_safe(listIterator,tempLstIterator,&(usedList)) {
        struct optEludeList * listNode=list_entry(listIterator, struct optEludeList, iulist);
        list_del(listIterator);
        free(listNode->usmPage);
        free(listNode);
    }
    // pthread_join.. of the aforedefined ones.
}

struct usm_ops dev_usm_ops= {
usm_setup:
    policiesSet1_setup,
usm_free:
    pol_structs_free
};