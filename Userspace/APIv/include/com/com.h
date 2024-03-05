/*
    - Le noyau et usm communiquent via des canaux.
    - usm crée des canaux au démarrage.
    - un canal a un type. Nous avons identifié des canaux file based (userfaultfd et procfs), RAM based (iouring), hybrid (uio drivers)
    - un canal est surveillé ou gérer par un worker. L'implantation de ce dernier dépend évidemment du type de canal.
    - un worker peut gérer plusieurs canaux de même type
    - les canaux transportent des événements
    - un canal a un sens de transport d'événements (user->kernel ou kernel->user). Un canal a d'autre propriété: priorité par rapport aux autres canaux, une taille, il ordonne ou pas les événements.
    - un événement a un type, un niveau de priorité, et d'autres paramètres
    - un worker est poll, périodique
*/

#include "event.h"
#include <errno.h>
#include <pthread.h>

// #define MAX_EVT 100



/*
    Définition des canaux
*/
enum usm_channel_type {FD_BASED, RAM_BASED, HYBRID};
enum usm_direction_type {USER2KERNEL, KERNEL2USER};
struct usm_channel_ops {
    int (*usm_init)();             // instantiate a poll_fd thingy?
    int (*usm_clean)();   // mode ?
    int (*usm_check)(int channel_id, int timeout);
    int (*usm_retrieve_evt)(struct usm_event * event);
    int (*usm_retrieve_evts) ();
    int (*usm_submit_evt) (struct usm_event * event);
    int (*usm_submit_evts) ();
    int (*usm_is_full) ();
};

typedef struct usm_channel {
    int usm_cnl_id;     // char..
    int fd; // char *file_name;//pour les fd_based
    char *buff;//pour les ram_based
    int usm_cnl_prio;//priority within the group
    enum usm_channel_type usm_cnl_type;
    enum usm_direction_type usm_drn_type;
    struct usm_channel_ops* usm_cnl_ops;
    //struct usm_channel* next;
} usm_channel;

typedef struct usm_channels_node {
    struct usm_channel *usm_channel;
    pthread_mutex_t chnmutex;
    struct list_head iulist;
} usm_channels_node;





/*
    Définition des workers
*/
enum usm_worker_type { POLL, PERIODIC}; // 'kay.. TBD
struct usm_worker_ops {
    int (*usm_init) ();
    int (*usm_clean) ();
    int (*usm_start) ();
    int (*usm_sort_evts) (); //trie les evts
    int (*usm_stop) ();
    int (*usm_handle_evt) (struct usm_event * event);  //traite l'evt
};
extern struct usm_worker {
    char * thread_name;
    pthread_t thread_id;
    int usm_period;
    int usm_budget;
    // struct usm_event usm_current_events[MAX_EVT]; // c'est ici que le worker place les evts des canaux qu'il gère lorsqu'il fait un tour de check
    struct list_head usm_current_events;
    int usm_current_events_length;
    struct list_head usm_channels;
    struct usm_worker_ops* usm_wk_ops;
    struct usm_worker* next;    // maybe do the same...
} usmWorkers;

// extern struct usm_channel_ops usm_cnl_userfaultfd_ops, usm_cnl_freed_ops, usm_cnl_new_process_ops;
extern struct usm_channel usm_cnl_userfaultfd, usm_cnl_freed_pgs, usm_cnl_nproc;
extern struct usm_worker usm_wk_uffd, usm_wk_free_pgs, usm_wk_nproc;

extern struct usm_channel_ops usm_cnl_userfaultfd_ops;

extern struct usm_channel_ops usm_cnl_freed_ops;

extern struct usm_channel_ops usm_cnl_new_process_ops;

extern void *usm_handle_evt_poll(void* wk_args);
extern void *usm_handle_evt_periodic(void* wk_args);
