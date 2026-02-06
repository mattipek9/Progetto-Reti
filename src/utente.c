#include "utente.h"


int main(int argc, char * argv[]){

    //associo l'handler libera_memoria al segnale SIGINT, per liberare correttamente la memoria allocata
    signal(SIGINT,libera_memoria);

    //randomizzo il seme
    srand(time(NULL));

    if (argc < 2) {
        printf("Uso: %s <porta>\n", argv[0]);
        exit(1);
    }
    
    int porta_utente = atoi(argv[1]);

    if(porta_utente <= LAVAGNA) {
        printf("Numero di porta non valido (deve essere > %d).\n", LAVAGNA);
        exit(1);
    }

    //creo i descrittori di file con pipe, per comunicazione tra worker e select
    if(pipe(pipefd) < 0){
        perror("Pipe: ");
        exit(1);
    }


    int fdmax = 0;             //Descrittore massimo
    fd_set master;             //Set principale di descrittori
    fd_set read_fds;           //Set temporaneo di descrittori per select

    //Socket TCP dedicato alla comunicazione con gli altri utenti
    int listener_user;         

    //Struttura per l'indirizzo dell'Utente (Listener)
    struct sockaddr_in my_addr_user; 


    // =========================================================================
    // 1. Configurazione e connessione alla Lavagna (Client verso Lavagna)
    // =========================================================================

    //Socket verso la Lavagna
    int sockfd = socket(AF_INET, SOCK_STREAM, 0); 
    if (sockfd < 0) {
        perror("Errore nella creazione del socket verso la Lavagna");
        exit(1);
    }
    
    //Indirizzo e porta della lavagna
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(LAVAGNA);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    //Connessione alla lavagna
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Errore nella connect alla Lavagna");
        exit(1);
    }

    //Registrazione alla lavagna
    if(invia_comando(sockfd,HELLO,porta_utente) < 0){
        printf("Errore hello utente\n");
        exit(1);
    };

    printf("Registrato alla lavagna\n");


    

    // =========================================================================
    // 2. Configurazione per l'ascolto di altri Utenti (Utente verso altri utenti)
    // =========================================================================
    
    // Creo il socket di ascolto per i messaggi dagli altri Utenti
    listener_user = socket(AF_INET, SOCK_STREAM, 0); 
    if (listener_user < 0) {
        perror("Errore nella creazione del listener per altri Utenti");
        exit(1);
    }

    //Setto il socket di ascolto sull'indirizzo e porta dell'Utente
    memset(&my_addr_user, 0, sizeof(my_addr_user));
    my_addr_user.sin_family = AF_INET;
    my_addr_user.sin_addr.s_addr = INADDR_ANY; // Ascolto su tutte le interfacce
    my_addr_user.sin_port = htons(porta_utente);

    //Associo il socket all'indirizzo e alla porta 
    if (bind(listener_user, (struct sockaddr*)&my_addr_user, sizeof(my_addr_user)) < 0){
        perror("Errore nella bind del listener Utente");
        exit(-1);
    }
    
    //Mi metto in ascolto
    if (listen(listener_user, 10) == -1){ 
        perror("Errore nella listen del listener Utente");
        exit(-1);
    } 

    printf("Utente in ascolto per altri Utenti sulla porta %d...\n", porta_utente);
    

    //stampa dei comandi disponibili
    printf("\nComandi disponibili:\n"
                "SHOW_LAVAGNA --> per visualizzare lo stato della lavagna\n"
                "CREATE_CARD <descrizione> --> per creare una card\n"
                "QUIT --> per uscire\n");

    // =========================================================================
    // 3. Loop principale con I/O Multiplexing
    // =========================================================================

    //Azzero i set e aggiungo i descrittori iniziali
    FD_ZERO(&master); 
    FD_ZERO(&read_fds); 
    
    //Standard Input (Console)
    FD_SET(0, &master); 

    //Aggiungo l'estremità di lettura della pipe al master
    FD_SET(pipefd[0],&master);
    
    //Socket di Connessione con la Lavagna
    FD_SET(sockfd, &master); 
    
    //Socket di Ascolto per gli altri Utenti
    FD_SET(listener_user, &master); 


    // Aggiorno il descrittore massimo 
    fdmax = (listener_user > sockfd) ? listener_user : sockfd;
    fdmax = (fdmax > 0) ? fdmax : 0;
    
    
    for(;;) {

        //timer per implementare il controllo sulle aste
        struct timeval timeout = {1,0};
       
        read_fds = master; 
        
        //Blocca finché non c'è attività su uno dei socket
        int activity = select(fdmax + 1, &read_fds, NULL, NULL, &timeout);

        if (activity < 0) {
            printf("Errore nella select...");
            continue;
        }

        //if(activity == 0)
        controlla_timeout_aste(sockfd,porta_utente);
            
        

        // Scorro tutti i descrittori per vedere chi è pronto
        for (int i = 0; i <= fdmax; i++) {
            if (FD_ISSET(i, &read_fds)) {

                if (i == 0) {
                    //L'Utente ha scritto sulla console (stdin)
                    leggi_e_invia_comando_utente(sockfd,porta_utente);

                } else if (i == listener_user) { 
                    //Nuova connessione in arrivo da un altro Utente, la accetto

                    struct sockaddr_in remote_addr;
                    socklen_t addr_size = sizeof(remote_addr);
                    int p2p_fd = accept(listener_user, (struct sockaddr *)&remote_addr, &addr_size);

                    if (p2p_fd == -1) {
                        perror("Errore nella accept P2P: ");
                    } else {
                        //mi salvo la porta del peer, mi serve per l'asta
                        int porta_peer;
                        if(recv(p2p_fd, &porta_peer, sizeof(int), 0) <= 0){
                            perror("Errore nella ricezione della porta del peer: ");
                            continue;
                        };
                        porta_peer = ntohl(porta_peer);

                        inserisci_porta(porta_peer, p2p_fd, &ports_u);

                        //aggiungo il nuovo socket al master
                        FD_SET(p2p_fd, &master);  

                        //aggiorno fdmax
                        if (p2p_fd > fdmax) {
                            fdmax = p2p_fd;
                        }
                        
                    }
                    
                } else if (i == sockfd) {
                    //Messaggio in arrivo dalla Lavagna 

                    leggi_comando_lavagna(sockfd,porta_utente);

                } else if (i == pipefd[0]){  
                    //significa che un lavoro è stato completato (il worker mi comunica che ha finito il lavoro)

                    //carta che è stata completata
                    int carta;    
                    read(pipefd[0], &carta, sizeof(int));

                    //invio card_done alla lavagna
                    card_done_u(sockfd,carta,porta_utente);

                } else {
                    //Messaggio in arrivo da un altro Utente già connesso (socket 'i'), mi manda solo il comando CHOOSE_USER
                    //questa funzione chiude anche la connessione con eventuali peer disconnessi
                    gestisci_choose(i,porta_utente,&master,sockfd);
                }
            }
        }
    }
    
    clean_utente();
    exit(0);
    
}