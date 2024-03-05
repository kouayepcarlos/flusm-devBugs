#include "../usm/list.h"
#include "../usm/list_sort.h"

// structure pour les événements à destination des politiques d'allocation. Il y aura la même chose pour les autres types de politique (eviction et oom)

enum evt_type { ALLOC, FREE, HINTS, VIRT_AS_CHANGE, PHY_AS_CHANGE, NEW_PROC, PROC_DTH };

struct usm_event {
    enum evt_type type;
    char * procName;
    // void *data;			// some other cool struct... mblr	// size..
    unsigned long vaddr;		//virt_addr;
    unsigned long length;
    unsigned long paddr;
    unsigned long flags;
    //void * info; 			// yup.. dang it.
    // int pfd; // could be called origin..
    int origin;
    struct list_head *channelNode;
};

typedef struct usm_events_node {
    struct usm_event * event;
    struct list_head iulist;
} usm_events_node;

/*struct usm_alloc_policy_event{
	enum evt_type { ALLOC, FREE, HINTS, VIRT_AS_CHANGE, PHY_AS_CHANGE, NEW_PROC },
	int process_pid,
	void *data;
};*/

/*struct usm_alloc_policy_alloc_event{
	long virt_addr,
	long length,//nombre de pages
	int origin,//origin of the event (page fault, mmap, etc.)
};

struct usm_alloc_policy_free_event{
	long virt_addr,
	long length, //nombre de pages
	int origin, //origin of the event (page fault, mmap, etc.)
};*/
// les autres types d'évts ici

//int usm_handle_alloc_events();
//int usm_handle_eviction_events();
//int usm_handle_oom_events();
