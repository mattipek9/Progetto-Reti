#include "shared.h"

#define SERVER_IP "127.0.0.1"          //IP del server, localhost nel nostro caso ovviamente 
#define WORK_DURATION 30               //Specifica la durata in secondi della simulazione del lavoro 
#define TIMEOUT_ASTA 15                //Specifica la durata in secondi del timeout, passato il quale l'asta si chiude forzatamente


//===============================================================================
// 1. Strutture dati 
//===============================================================================

//lista delle porte dei peer connessi 
struct port * ports_u;

//struttura dati per gestire le aste
struct statoAsta{ 
    int id_card;                      //Id della carta in asta
    int costo_minore;                 //Costo minore generato per la card (da me o da altri peer)
    int peer_vincente;                //Porta del peer vincente
    int risposte_ricevute;            //Numero di costi ricevuti
    int risposte_attese;              //Numero di costi attesi (dato dal numero di utenti inviato dalla lavagna con available_card_l)
    time_t inizio_asta;               //Inizio asta, per gestire disconnessioni utenti
    struct statoAsta * next; 
};

//aste attive a cui l'utente partecipa 
struct statoAsta * aste_attive = NULL;

//pipe che serve a comunicare al thread principale (la select) che un worker ha finito il suo lavoro
int pipefd[2]; 


//===============================================================================
// 2. Dichiarazioni 
//===============================================================================

//----funzioni progetto----
static void create_card_u(int sockfd,int porta_utente,char * descrizione);
static void card_done_u(int sockfd, int card_completata, int porta_utente);
static void ack_card(int sockfd,int card, int porta_utente);
static void available_card_u(int i, int porta_utente);
static void choose_user(int sockfd, int costo, int card_id);
static void stampa_lavagna(int sockfd);

//----funzioni comunicazione peer----
static int connetti_peer(int mia_porta, int porta_peer);
static void avvia_lavoro_su_card(int card);
static void *worker_thread(void *arg);

//----funzioni pulizia memoria----
static void clean_utente();
static void libera_memoria(int sig);

//----funzioni utilità----
static void leggi_e_invia_comando_utente(int sockfd, int porta_utente);
static void leggi_comando_lavagna(int sockfd, int porta_utente);

//----funzioni per gestione asta----
static struct statoAsta* crea_asta(int carta_id, int num_utenti, int mia_porta);
static void aggiungi_asta(struct statoAsta *nuova);
static struct statoAsta* trova_asta(int carta_id);
static void rimuovi_asta(int carta_id);
void controlla_timeout_aste(int sockfd, int porta_utente);

//----funzioni per gestire i costi inviati/ricevuti durante l'asta----
static void gestisci_choose(int i, int porta, fd_set * master, int sockfd);
static void invia_choose(int * utenti, int num_utenti, int mia_porta,int card_id, int sockfd);
static void decreta_vincitore(struct statoAsta * asta_attuale, int mia_porta, int sockfd);


//===============================================================================
// 3. Implementazione funzioni
//===============================================================================


//instaura una connessione TCP short-lived con un peer e invia la porta al peer (è una specie di hello con il peer)
int connetti_peer(int mia_porta, int porta_peer) {
    
    //Creazione nuovo socket TCP
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Errore creazione socket P2P");
        return -1;
    }

    //Configurazione indirizzo peer
    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(porta_peer);

    if (inet_pton(AF_INET,SERVER_IP, &remote_addr.sin_addr) <= 0) {
        perror("Errore inet_pton");
        goto cleanup;
    }

    //Tentativo di connessione con il peer
    if (connect(sockfd, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0) {
        perror("Errore nella connessione con il peer");
        goto cleanup;
    }

    //Invio la mia porta al peer
    int porta_net = htonl(mia_porta);
    ssize_t sent = send(sockfd, &porta_net, sizeof(int), 0);
    if (sent != sizeof(int)) {
        perror("Errore invio porta al peer");
        goto cleanup;
    }

    return sockfd; // Restituisce il fd P2P valido

cleanup:

    close(sockfd);
    return -1;
}


//invia il messaggio CHOOSE_USER con il costo a tutti gli utenti, le cui porte ci sono state inviate dalla lavagna
//con available_card_l()
void invia_choose(int * utenti,int num_utenti,int mia_porta,int card_id, int sockfd){

    //genero il costo per l'asta
    int costo = rand();

    struct statoAsta* asta = trova_asta(card_id);

    if(asta){
        //se l'asta è già presente, si tratta di un'asta incompleta creata in precedenza che deve essere 
        //completata con i dati mancanti forniti dall'available_card

         //gestisco pareggio tra il mio costo e quello che avevo ricevuto
        if(costo == asta->costo_minore){
            if(asta->peer_vincente > mia_porta){ 
                asta->peer_vincente = mia_porta;
            }
        } else if(costo < asta->costo_minore) {
            asta->peer_vincente = mia_porta;
            asta->costo_minore = costo;
        } 

        asta->risposte_attese = num_utenti;

    } else {
        //qua sono nel caso in cui non ho ricevuto alcun costo in precedenza, ed è arrivato prima l'available_card
        //creo correttamente l'asta
        struct statoAsta *nuova_asta = crea_asta(card_id, num_utenti, mia_porta);
        nuova_asta->costo_minore = costo;  
        aggiungi_asta(nuova_asta);  
        
    }


    for (int i = 0; i < num_utenti; i++) {

        int porta_target = utenti[i];
        int p2p_socket = connetti_peer(mia_porta, porta_target);

        if (p2p_socket > 0) {

            //Invio il messaggio CHOOSE_USER all'utente
            choose_user(p2p_socket, costo, card_id); 

        } else {
            // Se la connessione fallisce, l'utente potrebbe essersi disconnesso durante la connect per l'asta
            printf("Utente disconnesso durante l'asta...\n");
        } 
        
        //Chiudo la connessione TCP 
        close(p2p_socket); 
    }

    //se avevo già ricevuto tutti i costi e solo alla fine available_card, termino l'asta
    if(asta){
         decreta_vincitore(asta,mia_porta,sockfd);
         return;
    }

}


//funzione che esegue il thread che lavora sulla card passata per argomento, simulando con una sleep di 30s il lavoro.
//Una volta terminato il lavoro notifica la select attraverso la pipe
void *worker_thread(void *arg) {

    int carta = *(int*)arg;

    printf("Inizio lavoro sulla card %d...\n", carta);

    sleep(WORK_DURATION);   // simula lavoro

    printf("Finito lavoro sulla card %d!\n", carta);

    //notifica al thread principale 
    write(pipefd[1], &carta, sizeof(int));

    free(arg);

    return NULL;
}

//Avvia il lavoro su una card
void avvia_lavoro_su_card(int id_card){
    pthread_t th;

    int *id_ptr = malloc(sizeof(int)); 
    *id_ptr = id_card;

    pthread_create(&th, NULL, worker_thread, id_ptr);
    pthread_detach(th);
}

//gestisce la ricezione del messaggio CHOOSE_USER da parte di un altro peer
//aggiornando eventualmente il peer vincente, in caso in cui il costo_ricevuto sia minore di costo_minore
void gestisci_choose(int i,int porta,fd_set * master, int sockfd)
{
    int costo_net,card_net;
    int porta_mittente = trova_porta(i,ports_u);

    //rimuovo la porta del peer che tanto non mi serve più
    rimuovi_porta(porta_mittente,&ports_u);

    //ricevo id della card dal peer
     if(recv(i,&card_net,sizeof(int),0) <= 0){
        //il peer si è disconnesso 
        goto cleanup_peer;
    }
    card_net = ntohl(card_net);

    //ricevo il costo del peer
    if(recv(i,&costo_net,sizeof(int),0) <= 0){
        //il peer si è disconnesso 
        goto cleanup_peer;
    }
    costo_net = ntohl(costo_net);

    struct statoAsta * asta_attuale = trova_asta(card_net);
    if(!asta_attuale){
        //si tratta di un'asta per una card di cui non mi è acnora arrivato available_card dalla lavagna, creo
        //un'asta incompleta che si completerà poi
        struct statoAsta *asta_incompleta = malloc(sizeof(struct statoAsta));

        if (!asta_incompleta) return;
        
        asta_incompleta->id_card = card_net;
        asta_incompleta->peer_vincente = porta_mittente;
        asta_incompleta->costo_minore= costo_net;
        asta_incompleta->risposte_ricevute = 1;
        asta_incompleta->risposte_attese = 0;        
        asta_incompleta->inizio_asta = time(NULL);
        asta_incompleta->next = NULL;

        aggiungi_asta(asta_incompleta);
        
        return;
    }

    asta_attuale->risposte_ricevute++;
    
    //gestisco il caso di pareggio di costi, selezionando il peer con porta minore
    if(costo_net == asta_attuale->costo_minore){
        if(asta_attuale->peer_vincente > porta_mittente){ 
            asta_attuale->peer_vincente = porta_mittente;
        }
    } else if(costo_net < asta_attuale->costo_minore) {
        asta_attuale->peer_vincente = porta_mittente;
        asta_attuale->costo_minore = costo_net;
    } 

    //decreto eventuale vincitore
    decreta_vincitore(asta_attuale,porta,sockfd);
    return;

cleanup_peer: 

    close(i);
    FD_CLR(i,master);
    return;
}

//decreta il vincitore di un'asta in caso ci siano le condizioni per farlo e invia ACK alla lavagna
void decreta_vincitore(struct statoAsta * asta_attuale, int mia_porta, int sockfd){

    if(asta_attuale->risposte_attese == asta_attuale->risposte_ricevute){
        
        //invio ack_card alla lavagna se sono il vincitore
        if(asta_attuale->peer_vincente == mia_porta){

            //DEBUG
            printf("Ho vinto l'asta e mi occuperò della card %d\n", asta_attuale->id_card);

            ack_card(sockfd,asta_attuale->id_card,mia_porta);

            //inizio a lavorare sulla card
            avvia_lavoro_su_card(asta_attuale->id_card); 
        }

        rimuovi_asta(asta_attuale->id_card);
    }
}


//legge il comando digitato dall'utente e chiama la funzione opportuna per inviare il comando alla lavagna
void leggi_e_invia_comando_utente(int sockfd,int porta_utente) {
   
    //comando
    char comando[DIM_MAX_COMANDO];
    memset(comando, 0, sizeof(comando));

    //solo per la CREATE_CARD
    char descrizione[MAX_DESCR_ATT];
    memset(descrizione, 0, sizeof(descrizione));

 
    char totale[DIM_MAX_COMANDO + MAX_DESCR_ATT + 1];

    //prendo input da tastiera
    if (fgets(totale, sizeof(totale), stdin) == NULL) return;
    totale[strcspn(totale, "\n")] = 0; // Rimuove il newline
    

    //sscanf divide la riga in modo tale che la prima parola va in 'comando', 
    //e il resto (con gli spazi) va in 'descrizione'
    int n = sscanf(totale, "%s %99[^\n]", comando, descrizione);

    //nessun comando
    if (n < 1) return; 


    switch(Parsa_Comando(comando)){

        case QUIT:
            int ret = invia_comando(sockfd,QUIT,porta_utente);
            if(ret < 0){
                printf("Errore invio quit\n");
                exit(1);
            }

            clean_utente();
            exit(0);
        break;

        case CREATE_CARD:

            if(n < 2){
                printf("Il formato è CREATE_CARD <descrizione 100 caratteri max>\n");
            } else {
                create_card_u(sockfd,porta_utente,descrizione);
            }

        break;

        case SHOW_LAVAGNA:
            invia_comando(sockfd,SHOW_LAVAGNA,porta_utente);
        break;

        default:
            printf("Comando sconosciuto o non implementato, comandi disponibili:\n"
                   "SHOW_LAVAGNA --> per visualizzare lo stato della lavagna\n"
                   "CREATE_CARD <descrizione> --> per creare una card\n"
                   "QUIT --> per uscire\n");
        break;
    }
}


//legge il messaggio inviato dalla lavagna ed agisce di conseguenza
void leggi_comando_lavagna(int sockfd,int porta_utente){

    //leggo header
    struct header h;

    ssize_t ret = recv(sockfd,&h,sizeof(struct header),MSG_WAITALL);

    if (ret <= 0) {
        // La lavagna potrebbe essersi disconnessa
        if (ret == 0) {
            printf("La lavagna si è disconnessa!\n");
        } else {
            perror("Errore nella comunicazione con la lavagna: ");
        }
        close(sockfd);
        exit(-1);
    };

    //riconverto formato host (la porta utente ricevuta in questo caso non è significativa)
    h.comando = ntohl(h.comando);
    
    switch(h.comando)
    {
        case SHOW_LAVAGNA:
            stampa_lavagna(sockfd);
        break;

        case PING_USER:
            invia_comando(sockfd,PONG_LAVAGNA,porta_utente);
        break;

        case AVAILABLE_CARD:
            available_card_u(sockfd,porta_utente);
        break;

        default:
        break;                       
    };

}

//funzione che si occupa di liberare tutta la memoria allocata per l'utente
void clean_utente(){
    
    //dealloco le porte
    struct port * curr = ports_u;
    while(curr){
        struct port * next = curr->next;
        close(curr->fd);
        free(curr);
        curr = next;
    }

    //dealloco eventuali aste attive
    struct statoAsta *curr_asta = aste_attive;
    while(curr_asta){
        struct statoAsta *next = curr_asta->next;
        free(curr_asta);
        curr_asta = next;
    }
    
    //chiudo le estremità della pipe
    close(pipefd[1]);
    close(pipefd[0]);

}

//handler associato al SIGINT, setta terminato a true
void libera_memoria(int sig){
    (void)sig; 
    clean_utente();
    exit(0);
}


//Crea una nuova asta 
struct statoAsta* crea_asta(int carta_id, int num_utenti, int mia_porta) {

    struct statoAsta *asta = malloc(sizeof(struct statoAsta));
    if (!asta) return NULL;
    
    asta->id_card= carta_id;
    asta->peer_vincente = mia_porta;
    asta->risposte_ricevute = 0;
    asta->risposte_attese = num_utenti;
    asta->inizio_asta = time(NULL);
    asta->next = NULL;
    
    
    return asta;
}


//aggiunge l'asta alla lista delle aste
void aggiungi_asta(struct statoAsta *nuova) {
    if (!aste_attive) {
        aste_attive = nuova;
    } else {
        struct statoAsta *curr = aste_attive;
        while (curr->next) 
            curr = curr->next;
        curr->next = nuova;
    }
}


//trova l'asta per ID della carta in asta 
struct statoAsta* trova_asta(int carta_id) {
    struct statoAsta *curr = aste_attive;
    while (curr) {
        if (curr->id_card == carta_id) return curr;
        curr = curr->next;
    }
    
    return NULL;  //asta non trovata
}


//rimuovi l'asta completata dalla lista delle aste
void rimuovi_asta(int carta_id) {
    struct statoAsta *curr = aste_attive;
    struct statoAsta *prev = NULL;
    
    while (curr && curr->id_card != carta_id) {
        prev = curr;
        curr = curr->next;
    }
    
    if (!curr) return;
    
    if (prev) {
        prev->next = curr->next;
    } else {
        aste_attive = curr->next;
    }
    
    free(curr);  //dealloco l'asta
}

//funzione che controlla il timeout associato alle varie aste e in caso scada determina il peer vincente come quello attuale
void controlla_timeout_aste(int sockfd, int porta_utente) {

    struct statoAsta *curr = aste_attive;
    time_t ora = time(NULL);

    while (curr) {

        //Se l'asta è aperta da più di TIMEOUT_ASTA determino peer vincente
        if (ora - curr->inizio_asta >= TIMEOUT_ASTA) {
            printf("Timeout asta per card %d...\n", curr->id_card);
            
            if (curr->peer_vincente == porta_utente) {
                ack_card(sockfd,curr->id_card,porta_utente);
                avvia_lavoro_su_card(curr->id_card);
            }
            
            //Rimuovo l'asta
            int remove = curr->id_card;
            curr = curr->next; 
            rimuovi_asta(remove);

        } else {
            curr = curr->next;
        }
    }
}


//====================================================================
// 4. Funzioni del progetto
//====================================================================

//Funzione che riceve la lavagna in formato stringa e la stampa
void stampa_lavagna(int sockfd){
    int len;
    //Ricevo dimensione lavagna
    ssize_t ret = recv(sockfd, &len, sizeof(int), 0);
    if(ret < 0){
        perror("Errore ricezione lunghezza lavagna: ");
        exit(1);
    }

    len = ntohl(len);

    char buffer[len];
    
    //Ricevo la lavagna completa
    ret = recv(sockfd, buffer, len, MSG_WAITALL);
    if(ret < 0){
        perror("Errore ricezione lavagna: ");
        exit(1);
    }
    buffer[len] = '\0'; 
    
    //stampo la lavagna
    printf("%s", buffer);  
}


//invia il messaggio CREATE_CARD e permette all'utente di creare una card assegnandole la descrizione
void create_card_u(int sockfd,int porta_utente, char * descrizione){

    //invio header
    int ret = invia_comando(sockfd,CREATE_CARD,porta_utente);
    if(ret < 0) exit(1);

     
    //invio lunghezza stringa
    int lunghezza = strlen(descrizione);
    int lunghezza_net = htonl(lunghezza);
    ssize_t ret1 = send(sockfd,&lunghezza_net,sizeof(int),0);
    if(ret1 <= 0){
        perror("Errore nell'invio lunghezza stringa: ");
        exit(1);
    }
  
    //invio descrizione attività
    ret1 = send(sockfd,descrizione,lunghezza,0);
     if(ret1 <= 0){
        perror("Errore nell'invio dell'attività: ");
        exit(1);
    }
    
    return;
}


//invia CARD_DONE alla lavagna, con l'id della card completata
void card_done_u(int sockfd, int card_completata,int porta_utente){

    int ret = invia_comando(sockfd,CARD_DONE,porta_utente);
    if(ret < 0) exit(1);

    //invio id della card completata
    card_completata = htonl(card_completata);
    ssize_t ret1 = send(sockfd,&card_completata, sizeof(int),0);
    if(ret1 <= 0){
        perror("Errore nell'invio della card: ");
        exit(1);
    }

    return;
}

//invia ACK_CARD alla lavagna con l'id della card di cui mi occuperò
void ack_card(int sockfd,int card, int porta_utente){

    int ret = invia_comando(sockfd,ACK_CARD,porta_utente);
    if(ret < 0) exit(1);

    card = htonl(card);
    ssize_t ret1 = send(sockfd,&card, sizeof(int),0);
    if(ret1 <= 0){
        perror("Errore nell'invio della card: ");
        exit(1);
    }

}


//gestisce la ricezione del messaggio AVAILABLE_CARD
void available_card_u(int i, int porta_utente){

    int id_card;
    int num_utenti;

    if(recv(i,&id_card,sizeof(int), 0) <= 0){
        perror("Errore nella ricezione della card: ");
        return;
    }
    id_card = ntohl(id_card);


    if(recv(i,&num_utenti,sizeof(int), 0) <= 0){
        perror("Errore nella ricezione del numero di utenti: ");
        return;
    }
    num_utenti = ntohl(num_utenti);

   
    int utenti[num_utenti-1];
    int size = (num_utenti-1) * sizeof(int);

    if(recv(i,&utenti,size,0) <= 0){
        perror("Errore nella ricezione della lista utenti: ");
        return;
    }

    //converto array in formato host
    for (int i = 0; i < num_utenti - 1; i++) {
        utenti[i] = ntohl(utenti[i]);
    }

    //invia choose_user a tutti gli utenti
    invia_choose(utenti,num_utenti-1,porta_utente,id_card,i);

}

//comando choose_user per questa funzione non si utilizza il protocollo con l'header
//tanto gli utenti si inviano solo questo comando tra loro
void choose_user(int sockfd,int costo,int carta_id){

    carta_id = htonl(carta_id);
    ssize_t ret = send(sockfd,&carta_id, sizeof(int),0);
    if(ret <= 0){
        perror("Errore nell'invio dell'id della carta: ");
        exit(1);
    }

    costo = htonl(costo);
    ret = send(sockfd,&costo, sizeof(int),0);
    if(ret <= 0){
        perror("Errore nell'invio del costo: ");
        exit(1);
    }

}

