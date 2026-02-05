#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>

#define LAVAGNA 5678               //Porta lavagna
#define DIM_MAX_COMANDO 24         //Dimensione massima del comando 
#define MAX_DESCR_ATT 100          //Limite massimo di caratteri per la descrizione dell'attività


//===============================================================================
// 1. STRUTTURE DATI CONDIVISE
//===============================================================================

//struttura dati card
struct card{
    int id;                         //Id della card
    int colonna;                    //Colonna in cui si trova la card
    char attività[MAX_DESCR_ATT];   //Descrizione della card
    int utente;                     //Utente che svolge la card (significativo solo se colonna = 2)
    time_t doing_start;             //Timestamp di quando la card entra in doing
    time_t ping_time;               //Timestamp di quando la card riceve il ping
    bool ping_sent;                 //Ping inviato o meno
    struct card * pun;              
};

//struttura dati per implementare lista di porte utenti (sia connessi alla lavagna sia peer)
struct port{
    int port;                       //Porta utente
    int fd;                         //Endpoint di comunicazione
    struct port * next;             
};

//Comandi 
typedef enum{
    HELLO,
    QUIT,
    CREATE_CARD,
    MOVE_CARD,
    SHOW_LAVAGNA,
    SEND_USER_LIST,
    PING_USER,
    PONG_LAVAGNA,
    AVAILABLE_CARD,
    CHOOSE_USER,
    ACK_CARD,
    CARD_DONE,
    ERROR
} Tipo_Comando;


//header del messaggio
struct header{
    int port;                        //Porta di chi ha inviato il messaggio (significativa soprattutto per messaggi utente)     
    Tipo_Comando comando;            //Tipo di comando inviato
};


//===============================================================================
// 2. Funzioni di utilità condivise
//===============================================================================


//Funzione che invia l'header 
static inline int invia_comando(int sockfd, Tipo_Comando comando, int porta_utente){
    
    //inizializzo header, convertendo i campi per endianess (essendo int generalmente 4 byte e comando un enum quindi int)
    struct header h;
    h.comando = htonl((int)comando);
    h.port = htonl(porta_utente);

    //invio header (non stampo messaggi di errore, perchè saranno le varie funzioni che usano "invia_comando" a farlo)
    //in modo da discriminare la funzione che l'ha generato
    if (send(sockfd, &h, sizeof(struct header), 0) < 0) return -1;
    
    return 0;
}


//Traduce la stringa in un tipo enum per poter effettuare lo switch lato utente per input da tastiera
static inline Tipo_Comando Parsa_Comando(const char * str){
    
    if (str == NULL) {
        return ERROR;
    }
    
    if (strcmp(str, "HELLO") == 0) {
        return HELLO;
    } else if (strcmp(str, "QUIT") == 0) {
        return QUIT;
    } else if (strcmp(str, "CREATE_CARD") == 0) {
        return CREATE_CARD;
    } else if (strcmp(str, "MOVE_CARD") == 0) {
        return MOVE_CARD;
    } else if (strcmp(str, "SHOW_LAVAGNA") == 0) {
        return SHOW_LAVAGNA;
    } else if (strcmp(str, "SEND_USER_LIST") == 0) {
        return SEND_USER_LIST;
    } else if (strcmp(str, "PING_USER") == 0) {
        return PING_USER;
    } else if (strcmp(str, "PONG_LAVAGNA") == 0) {
        return PONG_LAVAGNA;
    } else if (strcmp(str, "AVAILABLE_CARD") == 0) {
        return AVAILABLE_CARD;
    } else if (strcmp(str, "CHOOSE_USER") == 0) {
        return CHOOSE_USER;
    } else if (strcmp(str, "ACK_CARD") == 0) {
        return ACK_CARD;
    } else if (strcmp(str, "CARD_DONE") == 0) {
        return CARD_DONE;
    } else {
        return ERROR;
    }
}

//Trova la porta dato il fd
static inline int trova_porta(int fd, struct port * port_list){
    struct port * p = port_list;

    while(p){
        if(p->fd == fd){
            return p->port;
        }
        p = p->next;
    }

    return 0;
}

//Rimuove la porta dalla lista delle porte
static inline void rimuovi_porta(int porta, struct port ** port_list){

    struct port * p = *port_list;

    if(!p) return;

    if(p->port == porta){
        *port_list = p->next;
        free(p);
        return;
    }

    struct port * prec = p;

    while(p && p->port != porta){
        prec = p;
        p = p->next;
    }

    if(!p) return;

    prec->next = p->next;
    free(p);
}

//Inserisce una porta nella lista delle porte
static inline void inserisci_porta(int porta, int i, struct port ** port_list){

    struct port * p = malloc(sizeof(struct port));
    if (!p) {
        perror("Errore malloc porta: ");
        return;
    }
    
    p->port = porta;
    p->next = NULL;
    p->fd = i;

    if(!*port_list){
        *port_list = p;
    } else {
        struct port * curr = *port_list;

        while(curr->next){
            curr = curr->next;
        }

        curr->next = p;
    }
}



