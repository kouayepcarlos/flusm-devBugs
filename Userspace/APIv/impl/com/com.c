//#include "include/com/com.h"
#include "../../include/usm/usm.h"
#include <poll.h>



void *usm_handle_evt_poll(void* wk_args) {                              // no need anymore of "poll" and opposed concepts.. they'll be in usm_check..
    struct usm_worker* wk=(struct usm_worker*)wk_args;
    struct list_head  *tempChnItr, *eventsListIterator, *tempEvtItr;
    struct usm_channels_node *channelsListIterator;
    struct usm_event * event;
    while(1) {
        list_for_each_entry(channelsListIterator,&(wk->usm_channels),iulist) {
            int notfn=channelsListIterator->usm_channel->usm_cnl_ops->usm_check(channelsListIterator->usm_channel->fd,0);
            if(notfn) {
#ifdef DEBUG
                printf("Event up!\n");
#endif
ralloc:
                event=malloc(sizeof(struct usm_event));
                if (!event) {
#ifdef DEBUG
                    printf("Failed malloc in usm_handle_evt_poll\n");
                    getchar();
#endif
                    goto ralloc;
                }
                event->origin=channelsListIterator->usm_channel->fd;
#ifdef DEBUG
                printf("Origin:%d\n\n",event->origin);
#endif
                if(likely(notfn>0)) {
                    if(likely(channelsListIterator->usm_channel->usm_cnl_ops->usm_retrieve_evt(event))) {
                        free(event);
                        continue;
                    }
                } else {
                    event->type=PROC_DTH;
                    event->channelNode=&channelsListIterator->iulist;
                }
                struct usm_events_node *eventListNode=(usm_events_node *)malloc(sizeof(usm_events_node));
                eventListNode->event=event;
                INIT_LIST_HEAD(&(eventListNode->iulist));
                list_add(&(eventListNode->iulist),&(wk->usm_current_events));
                wk->usm_current_events_length++;
            }
        }
        // wk->usm_wk_ops->usm_sort_evts(&wk->usm_channels);
        if(!(wk->usm_current_events_length))
            continue;
#ifdef DEBUG
        printf("Events number:%d\n",wk->usm_current_events_length);
#endif
        list_for_each_safe(eventsListIterator,tempEvtItr,&(wk->usm_current_events)) {
            struct usm_events_node * eventsIterator=list_entry(eventsListIterator, usm_events_node, iulist);
            if (!(wk->usm_wk_ops->usm_handle_evt(eventsIterator->event))) {
                wk->usm_current_events_length--;
                list_del(eventsListIterator);
                free(eventsIterator->event);
                free(eventsIterator);
            } else {
                //raise(SIGINT);
#ifdef DEBUG
                printf("Event not treated... dropping for now!\n");
                getchar();
#endif
                // list_move_tail(eventsListIterator,&(wk->usm_current_events));    | move it.. drop it... or wut?
                wk->usm_current_events_length--;
                list_del(eventsListIterator);
                free(eventsIterator->event);
                free(eventsIterator);
                continue;
            }
#ifdef DEBUG
            printf("\nEvent treated..\n");
#endif
        }
    }
}


/*usm_handle_evt_periodic(struct usm_worker* wk){           //.. this type of worker should be specialized/special enough to be launched scarcily ; one example'd be the thresholds' policies applier...
    int budget=wk->usm_budget;
    while(1){
again:
        // À implémenter: parcourir wk->usm_channels et placer les événements dans wk->usm_current_events
        budget--;
        if(budget!=0)
            goto again;
        wk->usm_wk_ops->usm_sort_evts();
        for(int i=0;i<wk->usm_current_events_length;i++){//on traite les evenements
            wk->usm_wk_ops->usm_handle_evt(wk->usm_current_events[i]);
        }
        wk->usm_current_events_length=0;
        sleep(5);
    }
}*/                                                         // what could be interesting would be a ring buffer... with a max. number in the loop collecting the events upon which we'd break....

int usm_uffd_ret_ev(struct usm_event * event) {
    struct uffd_msg msg;
#ifdef DEBUG
    printf("Retrieving UFFD event\n");
#endif
    int readres = read(event->origin,&msg,sizeof(msg));
    if(readres==-1) {
#ifdef DEBUG
        printf("[Mayday] Failed to get event\n");
#endif
        return errno;
    }
    // event->process_pid, event->origin to comm_fd, once channels loosened from processes
    switch (msg.event) {                 // dyent..
    case UFFD_EVENT_PAGEFAULT:
        event->type=ALLOC;
        event->vaddr=msg.arg.pagefault.address;
        // event->length=globalPageSize;        | obvious in the case of a page fault... the size of it'd be gPS, but it's accessible everywhere, so... ; but this should be needed/used later on.
        event->flags=msg.arg.pagefault.flags;
        break;
    case UFFD_EVENT_FORK:
        event->type=NEW_PROC;
        event->origin=msg.arg.fork.ufd;
        break;
    case UFFD_EVENT_REMAP:
        event->type=VIRT_AS_CHANGE;
        // old..new...addrs
        break;
    case UFFD_EVENT_UNMAP:  // this is not specifically true (uffd's way of handling remap...).. | toMod.
    case UFFD_EVENT_REMOVE: // possibly done by madvise...
        event->type=FREE;
        event->vaddr=msg.arg.remove.start;
        event->length=msg.arg.remove.end-msg.arg.remove.start;
        break;
    /*case UFFD_EVENT_:     // probably new ones later..
        event->type=__;
        break;*/
    default:
#ifdef DEBUG
        printf("[Mayday] Unspecified event\n");
#endif
        return 1;
    }
    return 0;
}

int usm_uffd_subm_ev(struct usm_event * event) {
    usm_set_pfn(event->origin,event->vaddr,event->paddr,0);
}

int usm_freed_page_ret_ev(struct usm_event * event) {
    unsigned long pfn = 0;
    int readres = read(usmFreePagesFd,&pfn,sizeof(pfn));
    if(readres==-1) {
#ifdef DEBUG
        printf("[Mayday] Failed to get freed page event\n");
#endif
        return errno;
    }
    event->paddr=pfn;
    event->type=PHY_AS_CHANGE;      // but free..? Wut the.... | dyend.
    // origin? Retrievable elsewhere too..
    return 0;
}

int usm_new_proc_ret_ev(struct usm_event * event) {
    char procName [16];
    int process = read(usmNewProcFd,&procName,sizeof(procName));
    if(process<=0) {
#ifdef DEBUG
        printf("[Mayday] Failed to get new process event\n");
#endif
        return errno;
    }
    event->procName=NULL;
#ifdef DEBUG
    printf("Received process' name : %s\n", procName);              /* TODO : investigate its disappearance in the cas of unnamed tasks (e.g. background saver of Redis).. just wth..*/
    getchar();
#endif
    event->origin=process;          // not really "origin" here but pfd.. meh...
    event->type=NEW_PROC;
    if(strlen(procName)!=0) {
        event->procName=malloc(sizeof(procName));
        strcpy(event->procName,procName);
    }
    write(usmNewProcFd,NULL,0);          // shan't be nec....
    return 0;
}

/*int usm_new_proc_subm_ev(struct usm_event * event, int comm_fd) {

}*/

int usm_poll_check(int channel_id, int timeout) {
    struct pollfd pollfd[1];
    pollfd[0].fd = channel_id;
    pollfd[0].events = POLLIN;
    int res=poll(pollfd,1,timeout);
    if(res<=0)       // basically undefined..
        return res;
    if(pollfd[0].events!=POLLIN || pollfd[0].revents!=POLLIN)
        return -1;
    return POLLIN;
}


struct usm_channel_ops usm_cnl_userfaultfd_ops= {
    .usm_retrieve_evt=   usm_uffd_ret_ev,
    .usm_submit_evt=     usm_uffd_subm_ev,
    .usm_check=          usm_poll_check
};

struct usm_channel_ops usm_cnl_freed_ops= {
    .usm_retrieve_evt=   usm_freed_page_ret_ev,
    .usm_check=          usm_poll_check
};

struct usm_channel_ops usm_cnl_new_process_ops= {
    // usm_init :
usm_retrieve_evt:
    usm_new_proc_ret_ev,
usm_check:
    usm_poll_check
    //usm_submit_evt:     usm_new_proc_subm_ev
};

//struct usm_channel usm_cnl_userfaultfd= {.usm_cnl_ops=usm_cnl_userfaultfd_ops};

struct usm_channel usm_cnl_freed_pgs;
struct usm_channel usm_cnl_nproc;

struct usm_worker usm_wk_uffd;
struct usm_worker usm_wk_free_pgs;
struct usm_worker usm_wk_nproc;
