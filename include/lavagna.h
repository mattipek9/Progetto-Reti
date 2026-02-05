#include "shared.h"


#define COL_WIDTH 30                //Larghezza colonne per la stampa della lavagna
#define NUM_CARD_INIZIALI 10        //Numero di card presenti nella lavagna all'avvio
#define ID_CARD_INIZIALE 11         //Id della card iniziale con cui inizializzare id_next, dato da NUM_CARD_INIZIALI + 1
#define PING_TIMEOUT 40             //Intervallo di tempo (in secondi) dopo il quale viene mandato ping alla card in doing
#define PING_RESPONSE 10            //Intervallo di tempo (in secondi) massimo concesso all'utente per rispondere al ping

//===============================================================================
// 1. STRUTTURE DATI
//===============================================================================


//variabile globale che mantiene il numero di utenti connessi alla lavagna
int n_utenti;

//liste volte a implementare la lavagna
struct card * to_do;
struct card * doing;
struct card * done;

//lista delle porte degli utenti connessi lato lavagna
struct port * ports;

//Serve per implementare il controllo nella lavagna nel caso in cui un utente abbia assegnate più di una card e quitti.
//In tal caso è necessario inviare available_card per far sì che il sistema non si blocchi 
int card_in_doing;

//Serve per gestire la stampa della lavagna in modo dinamico
int card_totali;

//id da assegnare alla prossima carta che verrà creata dall'utente
int id_next;

//===============================================================================
// 2. Dichiarazioni
//===============================================================================


//----funzioni progetto----
static void available_card_l(fd_set *master, int fd_max, int listener);
static void send_user_list(int i, int user[]);
static void ping_user(int i, fd_set *master);
static void send_lavagna(int i);

//----funzioni stampa lavagna----
static void prepara_lavagna(char * buffer);
static void show_lavagna();


//----funzioni pulizia memoria----
static void clean_lavagna();
static void libera_memoria(int sig);

//----funzioni utilità----
static struct card *has_card(int porta);
static int card_da_assegnare();
static void inserisci_card(struct card *card_, struct card **list);
static void rimuovi_card(int id, struct card **list);
static int* popola_array(int porta_destinataria);
struct card* trova_card(int id, struct card *lista);
static int print_wrapped(const char *str, char out[][COL_WIDTH+1]);
static void reset_ping(int i);



//===============================================================================
// 3. FUNZIONI DI UTILITA'
//===============================================================================


//controlla se un utente ha una card in doing prima della quit, in tal caso la card andrà spostata in to_do.
//restituisce un puntatore alla card o NULL
struct card * has_card(int porta){

    struct card * c = doing;
    while(c){   

        if(c->utente == porta){
            return c;
        }

        c=c->pun;
    }

    return NULL;
}

//serve ad aggiornare il fdmax nel caso in cui sia stato effettuato il quit dal client che aveva quell'fd
int aggiorna_fdmax(int fd_max,int listener,fd_set master){

    int nuovo_max = listener; //il listener è sempre attivo

    for (int i = 0; i <= fd_max; i++) { 
        if (FD_ISSET(i, &master)) {
            if (i > nuovo_max) {
                nuovo_max = i;
            }
        }
    }
    return nuovo_max;
}


//inserisce una card nella lista list
void inserisci_card(struct card * card_, struct card ** list){

    if(!*list){

        *list = card_;
        card_->pun = NULL;

    } else {

        struct card * curr = *list;

        while(curr->pun){
            curr = curr->pun;
        }

        curr->pun = card_;
        card_->pun = NULL;
    }
   
    return;
}

//inizializza la lavagna e inserisce 10 card iniziali per testing
void inizializza_lavagna(){

    //descrizione attività 
     char * descr[NUM_CARD_INIZIALI] = {
        "Pulire gli scarpini di Jamie Vardy",
        "Allenare i calci d'angolo",
        "Montare il campo per la partitella",
        "Chiamare il segretario della squadra ospite per capire in quale campo si gioca",
        "Parlare con il capitano",
        "Schierare la squadra al fantacalcio",
        "Salvarsi dalla retrocessione (Forza Viola)",
        "Studiare... :(",
        "Imparare esultanza di Gyokeres",
        "Mandare convocazione alla squadra sul gruppo whatsapp"
    };

    for(int i = 0; i < NUM_CARD_INIZIALI; i++){

        struct card *c = malloc(sizeof(struct card));
        if (!c) {
            perror("Errore malloc card: ");
            return;
        }
        
        memset(c, 0, sizeof(struct card));

        strncpy(c->attività, descr[i], MAX_DESCR_ATT - 1);
        c->attività[MAX_DESCR_ATT - 1] = '\0';

        c->colonna = 1;  //inizialmente la card è in to_do
        c->id = i+1;     //id della card incrementale
        c->utente = 0;   //inizialmente nessun utente svolge la card

        inserisci_card(c,&to_do);
        
    }

    doing = done = NULL;

    n_utenti = 0;  
    ports = NULL;
    card_in_doing = 0;
    card_totali = NUM_CARD_INIZIALI;
    id_next = ID_CARD_INIZIALE; //inizializzo a 11 perchè inserisco 10 card di default nella lavagna
}

//invia la lista delle porte degli utenti "User" ad un singolo utente con descrittore i
void send_user_list(int i,int user[]){

    size_t dim_array = (n_utenti-1) * sizeof(int);

    //converto in formato network
    for (int i = 0; i < n_utenti - 1; i++) {
        user[i] = htonl(user[i]);
    }

    if(send(i,user,dim_array,0) == -1){
        perror("Errore nell'invio delle porte: ");
        free(user);
        return;
    }

}


//funzione che stampa una stringa in blocchi di COL_WIDTH caratteri
//restituisce quanti "blocchi" sono stati stampati (quante righe)
int print_wrapped(const char *str, char out[][COL_WIDTH+1]) {
    int len = strlen(str);
    int lines = 0;
    int pos = 0;

    while (pos < len) {
        strncpy(out[lines], str + pos, COL_WIDTH);
        out[lines][COL_WIDTH] = '\0';
        lines++;
        pos += COL_WIDTH;
    }
    return lines;
}


//Prepara la lavagna in un buffer, con colonne fisse di dimensione COL_WIDTH e testo a capo (wrapping testuale)
void prepara_lavagna(char *buffer) {

    char *p = buffer;  // Puntatore che avanza nel buffer
    
    struct card *c1 = to_do;
    struct card *c2 = doing;
    struct card *c3 = done;

    int larghezza_lavagna = 3 * COL_WIDTH + 10;  // 3 colonne + 4 barre + 4 spazi

    //Header lavagna
    for (int i = 0; i < larghezza_lavagna; i++) p += sprintf(p, "=");
    p += sprintf(p, "\n");

    char title[256];
    snprintf(title, sizeof(title), "LAVAGNA - ID 5678");

    int pad_left  = (larghezza_lavagna - strlen(title)) / 2;
    int pad_right = larghezza_lavagna - strlen(title) - pad_left;

    for(int i = 0; i < pad_left; i++) p += sprintf(p, " ");
    p += sprintf(p, "%s", title);
    for(int i = 0; i < pad_right; i++) p += sprintf(p, " ");
    p += sprintf(p, "\n");

    for (int i = 0; i < larghezza_lavagna; i++) p += sprintf(p, "=");
    p += sprintf(p, "\n");

    //intestazione colonne
    p += sprintf(p, "| %-*s | %-*s | %-*s |\n",
           COL_WIDTH, "TO DO",
           COL_WIDTH, "DOING",
           COL_WIDTH, "DONE");

    //linea separatrice
    p += sprintf(p, "|-%.*s-|-%.*s-|-%.*s-|\n",
           COL_WIDTH, "------------------------------------------------------------",
           COL_WIDTH, "------------------------------------------------------------",
           COL_WIDTH, "------------------------------------------------------------");

    // stampa riga per riga
    while(c1 || c2 || c3) {

        char col1_text[512] = "";
        char col2_text[512] = "";
        char col3_text[512] = "";

        if(c1) {
            snprintf(col1_text, sizeof(col1_text), "ID:%d - %s", c1->id, c1->attività);
            c1 = c1->pun;
        }
        if(c2) {
            snprintf(col2_text, sizeof(col2_text), "ID:%d - %s (U:%u)", c2->id, c2->attività, c2->utente);
            c2 = c2->pun;
        }
        if(c3) {
            snprintf(col3_text, sizeof(col3_text), "ID:%d - %s (U:%u)", c3->id, c3->attività, c3->utente);
            c3 = c3->pun;
        }

        char col1_lines[30][COL_WIDTH+1];
        char col2_lines[30][COL_WIDTH+1];
        char col3_lines[30][COL_WIDTH+1];

        int n1 = print_wrapped(col1_text, col1_lines);
        int n2 = print_wrapped(col2_text, col2_lines);
        int n3 = print_wrapped(col3_text, col3_lines);

        int max_lines = n1;
        if(n2 > max_lines) max_lines = n2;
        if(n3 > max_lines) max_lines = n3;

        for(int i = 0; i < max_lines; i++) {
            p += sprintf(p, "| %-*s | %-*s | %-*s |\n",
                   COL_WIDTH, (i < n1 ? col1_lines[i] : ""),
                   COL_WIDTH, (i < n2 ? col2_lines[i] : ""),
                   COL_WIDTH, (i < n3 ? col3_lines[i] : ""));
        }

        // linea separatrice tra task
        p += sprintf(p, "|-%.*s-|-%.*s-|-%.*s-|\n",
               COL_WIDTH, "------------------------------------------------------------",
               COL_WIDTH, "------------------------------------------------------------",
               COL_WIDTH, "------------------------------------------------------------");
    }

    p += sprintf(p, "\n");
}


//rimuove la card id dalla lista l (non dealloca la card)
void rimuovi_card(int id, struct card ** l){

    struct card * c = *l;
    if(!c) return;

    if(c->id == id){
        *l = c->pun;
        return;
    }

    struct card * prec = NULL;

    while(c && c->id != id){
        prec = c;
        c = c->pun;
    }

    if(!c) return;

    prec->pun = c->pun;
}

//trova una card dato l'id
struct card* trova_card(int id, struct card *lista) {

    struct card *curr = lista;
    
    while (curr) {
        if (curr->id == id) {
            return curr; 
        }
        curr = curr->pun; 
    }

    //non trovata
    return NULL; 
}


//prepara l'array da inviare tramite la send_user_list, escludendo la porta del client destinatario
int* popola_array(int porta_destinataria){

    int *port_array = (int *)malloc((n_utenti-1) * sizeof(int));

    if (!port_array) {
        perror("Errore malloc array: ");
        return NULL;
    }

    struct port * curr = ports;
    int i = 0;

        while(curr){
            if(curr->port != porta_destinataria){
                port_array[i++] = curr->port;
            }
            curr = curr->next;
        }
    
    return (i > 0) ? port_array : NULL;
}

//resetto il ping di tutte le carte dell'utente i che mi ha mandato il pong 
void reset_ping(int i){
    struct card * curr = doing;

    int porta = trova_porta(i,ports);

    while(curr){ 
        if(curr->utente == porta && curr->ping_sent){
            curr->ping_sent = 0;
            curr->doing_start = time(NULL); // riparte il timer
        }
        curr = curr->pun;
    }
}

//trova il fd data la porta 
int trova_fd(int utente){
    struct port * p = ports;

    while(p){
        if(p->port == utente){
            return p->fd;
        }
        p = p->next;
    }

    return 0;
}


//invia il messaggio PING_USER all'utente i
void ping_user(int i,fd_set * master){
    
    int ret = invia_comando(i,PING_USER,0);
    if(ret < 0){
        FD_CLR(i, master);   // rimuovi dal set
        rimuovi_porta(trova_porta(i,ports), &ports);
        close(i);             // chiudi socket
    }
    
}

//funzione che libera la memoria per la lavagna 
void clean_lavagna(){

    printf("=== Pulizia memoria lavagna ===\n");
    
    int count_todo = 0, count_doing = 0, count_done = 0, count_ports = 0;
    
    //libera to_do
    struct card * curr_card = to_do;
    while(curr_card){
        struct card * next = curr_card->pun;
        free(curr_card);
        count_todo++;
        curr_card = next;
    }
    
    //libera doing
    curr_card = doing;
    while(curr_card){
        struct card * next = curr_card->pun;
        free(curr_card);
        count_doing++;
        curr_card = next;
    }
    
    //libera done
    curr_card = done;
    while(curr_card){
        struct card * next = curr_card->pun;
        free(curr_card);
        count_done++;
        curr_card = next;
    }
    
    //libera lista porte
    struct port * curr_port = ports;
    while(curr_port){
        struct port * next = curr_port->next;
        close(curr_port->fd);  
        free(curr_port);
        count_ports++;
        curr_port = next;
    }
    
    printf("Deallocate: %d card (to_do) + %d card (doing) + %d card (done) + %d porte\n",
           count_todo, count_doing, count_done, count_ports);
    printf("Totale memoria liberata: ~%lu bytes\n", 
           (count_todo + count_doing + count_done) * sizeof(struct card) + 
           count_ports * sizeof(struct port));
    printf("=== Cleanup completato ===\n");
}

//handler associato alla gestione del segnale SIGINT, chiama la funzione per liberare la memoria
void libera_memoria(int sig){
    (void)sig;   //sere per evitare il warning "parameter unused"
    clean_lavagna();
    exit(0);
}



int card_da_assegnare(){

    struct card * c = to_do;
    while(c){

        if(c->colonna == 1){
            c->colonna = 2;
            return c->id;
        }
        
        c = c->pun;
    }

    return -1;
}

//====================================================================
// 3. FUNZIONI DEL PROGETTO
//====================================================================

//funzione per stampa della lavagna, con stima dinamica della dimensione della stessa
void show_lavagna(){
    int grandezza_stimata = 500 + card_totali * 1000;
    char * buf = malloc(grandezza_stimata);
    prepara_lavagna(buf);
    printf("%s", buf);
    free(buf);
}

//invia la lavagna all'utente con socket i per stamparla
void send_lavagna(int i) {

    int ret = invia_comando(i,SHOW_LAVAGNA,5678);
    if(ret < 0) exit(1);

    //preparo la lavagna
    int grandezza_stimata = 500 + card_totali * 1000;
    char * buf = malloc(grandezza_stimata);
    prepara_lavagna(buf);
    
    //dimensione effettiva lavagna
    int len = strlen(buf);

    int len_net = htonl(len);
    ssize_t ret1 = send(i, &len_net, sizeof(int), 0);
    if(ret1 < 0){
        perror("Errore invio dimensione lavagna: ");
        return;
    }

    ret1 = send(i, buf, len, MSG_WAITALL);
    if(ret1 < 0){
        perror("Errore invio lavagna: ");
        return;
    }

    free(buf);
}



//gestisce la ricezione del comando create_card
void create_card_l(int i, fd_set * master, int fd_max, int listener){

    int len;
    char attività[MAX_DESCR_ATT];
    memset(attività, 0, sizeof(attività));

    if(recv(i,&len,sizeof(int), 0) <= 0){
        perror("Errore nella ricezione della lunghezza: ");
        return;
    }
    len = ntohl(len);

    if(recv(i,attività,len, 0) <= 0){
        perror("Errore nella ricezione dell'attività: ");
        return;
    }
    
    attività[len] = '\0'; 

    //alloco la card
    struct card *c = malloc(sizeof(struct card));
    if (!c) {
        perror("Errore malloc card: ");
        return;
    }

    memset(c, 0, sizeof(struct card));

    //la inizializzo
    c->id = id_next++;
    c->colonna = 1;
    strncpy(c->attività,attività,len);

    card_totali++;

    //inserisco la card
    inserisci_card(c,&to_do);

    //stampo stato lavagna
    int grandezza_stimata = 500 + card_totali * 1000;
    char * buf = malloc(grandezza_stimata);
    prepara_lavagna(buf);
    printf("%s",buf);
    free(buf);
    

    //DEBUG
    printf("Card Inserita!\n");

    
    //se non ci sono card in to_do, però potrebbero esserci almeno due utenti, quindi l'inserimento della card
    //potrebbe rendere vere le condizioni di available_card
    available_card_l(master,fd_max,listener);
       
}


//Gestisce la ricezione di un quit da parte dell'utente. Il campo voluntary serve a distinguere la gestione della classica quit
//voluntary = true, dalla disconnessione inaspettata voluntary = false
void quit_l(int i,bool voluntary,int porta_utente){

    if(voluntary){

        struct card * c = has_card(porta_utente);

        //rimuovo tutte le card che l'utente aveva in doing e le rimetto in to_do
        while(c){
            rimuovi_card(c->id,&doing);
            inserisci_card(c,&to_do);
            card_in_doing--;

            show_lavagna();

            c = has_card(porta_utente);
        }

        rimuovi_porta(porta_utente,&ports);

        //DEBUG
        /*struct port * curr2 = ports;
        printf("Ecco le porte che ho memorizzato:\n"); 
        while(curr2){
            printf("%d\n", curr2->port);
            curr2 = curr2->next;
        } */
    }
    
    //in caso di disconnessione inaspettata mi limito a decrementare il numero di utenti e chiudere il socket
    //il resto della gestione avviene successivamente
    n_utenti--;
    close(i);

    return;
}

//invia a tutti gli utenti connessi la prima card in to_do, il numero di utenti e la lista delle porte degli utenti connessi
void available_card_l(fd_set *master, int fd_max, int listener){

    //controllo condizione per available_card
    if(n_utenti < 2 || card_in_doing >= n_utenti - 1) return; 

    //controllo se ci sono card da assegnare
    int work_card = card_da_assegnare();
    if(work_card < 0){
        return;
    };

    //DEBUG
    printf("[LAVAGNA] Invio AVAILABLE_CARD a %d utenti per card ID:%d - timestamp: %ld\n", n_utenti, work_card, time(NULL));
    
    for(int i = 0; i <= fd_max; i++){

        if(FD_ISSET(i,master) && i != listener){
            
            //DEBUG
            int porta_utente = trova_porta(i,ports);
            printf("[LAVAGNA] -> Invio a utente %d (fd=%d)\n", porta_utente, i);


            //invia header
            int ret = invia_comando(i,AVAILABLE_CARD,5678);
            if(ret < 0){
                //la gestione dell'errore è questa e si completa poi nella send ping
                //qua levo il socket dal pool master e abbasso il numero di utenti per far svolgere correttamente l'asta 
                //in maniera tale che gli altri peer sappiano che un utente si è disconnesso inaspettatamente
                FD_CLR(i,master);
                rimuovi_porta(trova_porta(i,ports),&ports);
                n_utenti--;
                continue;
            }

            //invia l'id della card
            int id_net = htonl(work_card);
            if (send(i,&id_net, sizeof(int), 0) == -1) {
                FD_CLR(i,master);
                rimuovi_porta(trova_porta(i,ports),&ports);
                n_utenti--;
                continue;
            }


            //invia il numero di utenti
            int n_utenti_net = htonl(n_utenti);
            if(send(i,&n_utenti_net,sizeof(int),0) == -1){
                FD_CLR(i,master);
                rimuovi_porta(trova_porta(i,ports),&ports);
                n_utenti--;
                continue;
            }

        
            //invia la lista degli utenti 
            int porta_dest = trova_porta(i,ports); //cerco la porta del destinatario
            int* to_send = popola_array(porta_dest); 

            send_user_list(i,to_send);
            free(to_send);
            

            //DEBUG
            printf("[LAVAGNA] -> Inviato completamente a utente %d\n", porta_utente);
        }

    }

}

//Gestisce la ricezione del messaggio ACK_CARD, assegnando la card all'utente identificato da porta_ricevuta
void move_card(int i, int porta_utente){

    int card_ricevuta;
    int bytes_read = recv(i,&card_ricevuta,sizeof(int),0);
    if (bytes_read <= 0) {
        perror("Errore nella move_card: ");
        return;
    }

    card_ricevuta = ntohl(card_ricevuta);

    struct card * c = trova_card(card_ricevuta,to_do);
    if(!c) return;

    //modifico i campi in quanto la carta passerà in doing, svolta dall'utente porta_ricevuta
    c->colonna = 2;
    c->utente = porta_utente;
    card_in_doing++;

    //avvio il timer sulla card per il PING
    c->doing_start = time(NULL);
    c->ping_sent = false;

    int id = c->id; //id della carta da assegnare 
    
    rimuovi_card(id,&to_do);
    inserisci_card(c,&doing);

    //mostro la lavagna
    show_lavagna();
    
    //DEBUG
    printf("Card spostata\n");
}

//gestisce la ricezione del messaggio CARD_DONE, cercando la carta completata dall'utente e spostandola in done
void card_done_l(int i){

    int card_completata;
    int bytes_read = recv(i,&card_completata,sizeof(int),0);
    if (bytes_read <= 0) {
        perror("Errore nella card_done_l: ");
        return;
    }
    card_completata = ntohl(card_completata);
    
    //trovo la carta
    struct card * c = trova_card(card_completata,doing);
    if(!c) return;
   
    //sposto la card in done
    c->colonna = 3;
    rimuovi_card(card_completata,&doing);
    inserisci_card(c,&done);
    card_in_doing--;

    //mostro la lavagna
    show_lavagna();
    
}


//controlla per le card in doing se è necessario inviare ping_user (ossia se sono trascorsi 40 secondi da quando
//la card è entrata in doing). 
void send_ping(fd_set * master,int fdmax,int listener){

    time_t now = time(NULL);
    struct card * curr = doing;
   
    while(curr){

        int fd = trova_fd(curr->utente); 
        struct card * next = curr->pun;

        //dopo 40s -> PING_USER
        if (!curr->ping_sent &&
            now - curr->doing_start >= PING_TIMEOUT) {

            //DEBUG
            printf("La card %d è da troppo tempo in doing, mando il ping\n",curr->id);

            ping_user(fd,master);

            curr->ping_sent = 1;
            curr->ping_time = now;
        }


        //dopo 10s dal ping, se l'utente non ha risposto con la PONG, la card torna in to_do
        if (curr->ping_sent &&
            now - curr->ping_time >= PING_RESPONSE) {

            rimuovi_card(curr->id,&doing); //rimuovo la card da doing

            curr->colonna = 1;
            curr->ping_sent = false;
            curr->utente = 0;
            curr->pun = NULL;
            curr->doing_start = 0;

            inserisci_card(curr,&to_do); //e la rimetto in to_do
            card_in_doing--;
            
            //mostro la lavagna
            show_lavagna();

            available_card_l(master,fdmax,listener);
        }

        curr = next;
    }
 
}




