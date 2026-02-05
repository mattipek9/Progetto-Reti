#include "lavagna.h"


int main(){

    //Associo l'handler libera_memoria al segnale SIGINT, per liberare correttamente la memoria allocata
    signal(SIGINT,libera_memoria); 

    inizializza_lavagna();
    
    //Numero massimo di descrittori
    int fdmax = 0;  

    //Set principale
    fd_set master; 

    //Set di lettura
    fd_set read_fds; 

    //Socket per l'ascolto
    int listener; 

    //Azzero i set
    FD_ZERO(&master); 
    FD_ZERO(&read_fds); 

    struct sockaddr_in my_addr, client_addr;

    //Socket TCP 
    listener = socket(AF_INET, SOCK_STREAM, 0); 
    if (listener < 0) perror("socket");

    //Setto il socket
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = INADDR_ANY; //mi metto in ascolto su tutte le interfacce
    my_addr.sin_port = htons(LAVAGNA);
  
    //Per evitare che una volta terminata la lavagna, se questa viene rimandata subito in esecuzione 
    //si ottenga l'errore "Address already in use"
    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Errore in setsockopt: ");
        exit(-1);
    }
    
    //effettuo il binding
    if (bind(listener, (struct sockaddr*)&my_addr, sizeof(my_addr)) < 0){
        perror("Errore nella bind: ");
        exit(-1);
    }
        

    int ret = listen(listener,10);
    if(ret == -1){
        perror("Errore nella listen: ");
        exit(-1);
    } 

    printf("Lavagna in ascolto su porta %d...\n", LAVAGNA);

    show_lavagna();

    //Aggiungo il listener al set
    FD_SET(listener,&master); 
    //Tengo traccia del maggiore
    fdmax = listener; 

    for(;;) {

        //timer per implementare il ping
        struct timeval timeout = {1,0};
       
        read_fds = master;  
        int activity = select(fdmax + 1, &read_fds, NULL, NULL, &timeout); 
        
        if (activity < 0){
            printf("Errore nella select...");
            continue;
        }

        //Ping 
        send_ping(&master,fdmax,listener);


        for (int i = 0; i <= fdmax; i++) {

            if (FD_ISSET(i, &read_fds)) {
                
                
                if (i == listener) {  
                    //I dati li ha il listener, dunque significa che è arrivata una connect dal client
                    
                    //Nuova connessione
                    socklen_t addrlen = sizeof(client_addr);
                    int newfd = accept(listener, (struct sockaddr *)&client_addr, &addrlen);
                    FD_SET(newfd, &master);
                    if (newfd > fdmax) fdmax = newfd;

                } else {
                    //I dati li ha il client

                    //Per prima cosa leggo l'header
                    struct header head;
                    
                  
                    //Ricevo l'header del messaggio
                    ssize_t bytes_read = recv(i,&head,sizeof(struct header),0);
                    
                    //ricevuto header incompleto a causa di ctrl+c del client
                    if(bytes_read > 0 && bytes_read < (ssize_t)sizeof(struct header)){
                        printf("Ricevuto header incompleto da %d, lo ignoro...\n", i);
                        continue;
                    }
                    
                    //se fallisce l'utente potrebbe essersi disconnesso inaspettatamente, volendo però testare la funzionalità
                    //della ping controllo se l'utente ha carte in doing e in questo caso non le sposto, lasciando che sia 
                    //il medesimo meccanismo (ping) a farlo.
                    if (bytes_read <= 0) { 

                        quit_l(i,false,0);

                        //Rimuovi l'FD dal set master.
                        FD_CLR(i, &master); 
            
                        //Se i == fd_max, devo aggiornare fd_max
                        if (i == fdmax) fdmax = aggiorna_fdmax(fdmax,listener,master);

                        //Rimuovo la porta dell'utente
                        int quit = trova_porta(i,ports);
                        rimuovi_porta(quit,&ports);

                        continue;
                    }

                    //converto a formato host long
                    head.comando = ntohl(head.comando);
                    head.port = ntohl(head.port);


                    switch(head.comando)
                    {
                        case HELLO:
                            printf("HELLO utente %d\n", head.port);

                            //aggiungo l'utente
                            n_utenti++;
                            inserisci_porta(head.port,i,&ports);

                            //dopo una hello possono esserci >1 utenti connessi, perciò chiamo available_card
                            available_card_l(&master,fdmax,listener);
                        break;

                        case QUIT:
                            printf("QUIT utente %d\n", head.port);

                            //In questo caso il secondo argomento è true perchè la disconnessione dell'utente è volontaria
                            quit_l(i,true,head.port);  
                            
                            //Rimuovo l'FD dal set master.
                            FD_CLR(i, &master); 
            
                            //Se i == fdmax, devo aggiornarlo
                            if (i == fdmax) fdmax = aggiorna_fdmax(fdmax,listener,master);

                            //per evitare che quando un utente vince tutte le card e quitta il sistema si blocchi
                            //chiamo available card
                            if(card_in_doing < n_utenti-1)
                                available_card_l(&master,fdmax,listener);
                            
                        break;
                    
                        case SHOW_LAVAGNA:
                            printf("SHOW_LAVAGNA utente %d\n", head.port);

                            //invio la lavagna all'utente
                            send_lavagna(i);
                        break;

                        case ACK_CARD:
                            printf("ACK_CARD utente %d\n", head.port);
                            
                            //se la lavagna riceve l'ack allora sposta la card in doing
                            move_card(i,head.port);
                        break;

                        case CARD_DONE:
                            printf("CARD_DONE utente %d\n", head.port);
                            card_done_l(i);

                            //controllo se una volta completata una card ci sono le condizioni per iniziare
                            //a gestirne un'altra
                            available_card_l(&master,fdmax,listener);
                        break;

                        case CREATE_CARD:
                            printf("CREATE_CARD utente %d\n", head.port);
                            create_card_l(i,&master,fdmax,listener);  
                        break;

                        case PONG_LAVAGNA:
                            printf("PONG_LAVAGNA utente %d\n", head.port);
                            //nel caso in cui la lavagna riceva il pong, si limita a riattivare il timer per la card 
                            reset_ping(i);
                        break;

                        default:

                        break;
                        
                    };                                             
                }                  
            }   
        }
    }
}