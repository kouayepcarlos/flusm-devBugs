#include "../../include/usm/usm.h" 		// toMod..

#ifdef DEBUG
bool hsh_iter(const void *item, void *udata) {
    const struct usm_process_link *user = item;
    printf("%s (pol=%ld)\n", user->procName, user->pol);
    return true;
}
#endif

int usm_handle_events(struct usm_event *evt) {
    int ret = 0;
#ifdef DEBUG
    printf("Event : %d\n",evt->type);
#endif
    switch (evt->type) {
    case ALLOC:
#ifdef DEBUG
        if(evt->flags&UFFD_PAGEFAULT_FLAG_MINOR){
                printf("Minor flag received! Simply continuing for now.\n");      /*Should theoretically always be about shared pages or similar.. as the kernel shouldn't be handling any swapping ; to enhance perf., we could use its swp_entries.. and either put direct accesses to USM's swap file, or a little dummy (which we should be doing if SPDK to be used..).. */
                getchar();
        }
        else
                printf("Trying to allocate..\n");
        if(evt->flags&UFFD_PAGEFAULT_FLAG_WRITE)
                printf("Was a write attempt!\n");
        else
                printf("Was a read attempt!\n");
#endif
        if(unlikely(evt->flags&UFFD_PAGEFAULT_FLAG_MINOR))
                ret = usm_continue(evt);
        else
                ret = usmPolicyDefinedAlloc(evt);
        // USM tells them we'd need... things.... through evt #SpatialDistrib...., and if he doesn't..... uhhh, later.
        if (unlikely(ret)) {
#ifdef DEBUG
            printf("[Sys] Failed allocation!\n");       // basically full memory, but could be related to a failed submission... #toMod./Complify
#endif
            // immediate swapping or OOM bhvr code.. +goto TODO            // gotta be really extremely rare.. -_-'.. with a good swapping policy obviously...
        }
#ifdef DEBUG
        printf("Memory consumed : %.2f%s #freePage\n", usedMemory/1024/1024>0?(float)usedMemory/1024/1024:(float)usedMemory/1024, usedMemory/1024/1024>0?"MB":"KB");    // should be done by dev.?
#endif
        break;
    case PHY_AS_CHANGE:
#ifdef DEBUG
        printf("Received %lu ! #freePage\n", evt->paddr);
#endif
        ret=usmPolicyDefinedPageIndexFree(evt);			// struct page...! 'event.....
#ifdef DEBUG
        printf("Memory consumed : %.2f%s #freePage\n", usedMemory/1024/1024>0?(float)usedMemory/1024/1024:(float)usedMemory/1024, usedMemory/1024/1024>0?"MB":"KB");    // should be done by dev.?
#endif
        break;
    case FREE:
        //ret = usmPolicyDefinedFree(evt);		// origin'll be compliant..
        break;
    case VIRT_AS_CHANGE:
        // remapping... call swapping code or simply update all relevant fields (e.g. virt. addresses in pages..)
        break;
    case NEW_PROC:
        // assignment strat... +workerChoice..RandifNotDefined(will be treated in choseWorker)
        usmSetupProcess(evt);
        /*
        	for uffd|usm, there's, for the moment, one process per channel.... quite easily alterable though.
        */
        appendUSMChannel(&usm_cnl_userfaultfd_ops,evt->origin,&(usmChooseWorker()->usm_channels));              // inside usmSetupProcess?
#ifdef DEBUG
        printf("\tProcess %d welcomed by a random worker.\n",evt->origin/*,chosenWorker*/);
#endif
        // We could wake the process here instead...
        break;
    case PROC_DTH:
        // cleaning..
        struct usm_channels_node * channelNode=list_entry(evt->channelNode, struct usm_channels_node, iulist);
        pthread_mutex_lock(&channelNode->chnmutex);
        list_del(evt->channelNode);
        pthread_mutex_unlock(&channelNode->chnmutex);							// absolutely unsufficient... prev. and next're involved.. freak it man...! But... are they problematic...
        free(channelNode->usm_channel);
        free(channelNode);
        // no need to check all them page table elements... as they'll be received either way!
        resetUsmProcess(evt->origin);					// bound to create some troubles if lock not used....
#ifdef DEBUG
        printf("Process %d just reached the afterworld.\n",evt->origin);
#endif
        break;
    default:
#ifdef DEBUG
        printf("Weird event.. \n");
#endif
        ret=1;
        break;
    }
    return ret;
}

int events_compare(void *priv, const struct list_head *a, const struct list_head *b) {
    return usmGetEventPriority(list_entry(a,usm_events_node,iulist)->event)<usmGetEventPriority(list_entry(b,usm_events_node,iulist)->event)?1:0;
}

int usm_sort_events(struct list_head *events) {
    list_sort(NULL,events,events_compare);
}